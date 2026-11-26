#include "tasks_store.h"

#include <algorithm>

#include "../../hal/sdcard.h"
#include "../../os/storage.h"

namespace {
// on the microSD card
const char* kBase = "/cardputerOS/tasks/data.json";
const char* kPending = "/cardputerOS/tasks/pending.json";
const char* kState = "/cardputerOS/tasks/state";
const char* kNoCard = "No microSD card. Tasks keeps its data on the card: insert one and try again.";
constexpr size_t kMaxDone = 200;  // done tasks listed (the most recently completed)
fs::FS& sd() { return hal::sdFs(); }

bool readText(const char* path, std::string& out) {
    out.clear();
    if (!sd().exists(path)) return false;
    File f = sd().open(path, "r");
    if (!f) return false;
    out.resize(f.size());
    const size_t n = out.empty() ? 0 : f.read((uint8_t*)&out[0], out.size());
    out.resize(n);
    f.close();
    return true;
}

// Written to a temporary file first, so a power cut or a pulled card cannot leave half a file.
bool writeText(const char* path, const std::string& data) {
    const std::string tmp = std::string(path) + ".tmp";
    File f = sd().open(tmp.c_str(), "w");
    if (!f) return false;
    const size_t n = f.write((const uint8_t*)data.data(), data.size());
    f.close();
    if (n != data.size()) {
        sd().remove(tmp.c_str());
        return false;
    }
    if (sd().exists(path)) sd().remove(path);
    return sd().rename(tmp.c_str(), path);
}
}  // namespace

const char* TasksStore::kDir = "/cardputerOS/tasks";

// Keeps only the kMaxDone most recently completed done tasks (and any with pending edits).
void TasksStore::trimDone(std::vector<mindwtr::TaskRow>& tasks) const {
    std::vector<int64_t> doneKeys;
    for (auto& t : tasks)
        if (t.status == "done" && !pendingFor(t.id)) doneKeys.push_back(t.completedKey);
    if (doneKeys.size() <= kMaxDone) return;
    std::nth_element(doneKeys.begin(), doneKeys.begin() + (doneKeys.size() - kMaxDone), doneKeys.end());
    const int64_t cutoff = doneKeys[doneKeys.size() - kMaxDone];
    size_t dropAtCutoff = std::count(doneKeys.begin(), doneKeys.end(), cutoff) -
                          std::count(doneKeys.end() - kMaxDone, doneKeys.end(), cutoff);
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
                               [&](const mindwtr::TaskRow& t) {
                                   if (t.status != "done" || pendingFor(t.id)) return false;
                                   if (t.completedKey < cutoff) return true;
                                   if (t.completedKey == cutoff && dropAtCutoff) {
                                       dropAtCutoff--;
                                       return true;
                                   }
                                   return false;
                               }),
                tasks.end());
}

bool TasksStore::index(const char* path, std::vector<mindwtr::TaskRow>& tasks, std::vector<mindwtr::Project>& projects,
                       json::ElementScanner::ArrayInfo& tasksArray, std::string& err) const {
    tasks.clear();
    projects.clear();
    File f = sd().open(path, "r");
    if (!f) {
        err = "Could not open the task file on the SD card.";
        return false;
    }
    size_t doneSeen = 0;
    json::ElementScanner scanner(
        {"tasks", "projects"}, [&](const std::string& key, size_t start, size_t end, const std::string& text, bool truncated) {
            if (truncated) return;  // a single task over 32 KB: left alone
            json::Value v;
            if (!json::parse(text, v)) return;
            if (key == "tasks") {
                mindwtr::TaskRow t;
                if (!mindwtr::summarize(v, t) || t.deleted) return;
                if (t.status == "archived" && !pendingFor(t.id)) return;  // never shown
                t.start = (uint32_t)start;
                t.end = (uint32_t)end;
                if (t.status == "done" && ++doneSeen > 2 * kMaxDone) {
                    trimDone(tasks);  // bound memory while scanning a long history
                    doneSeen = kMaxDone;
                }
                tasks.push_back(std::move(t));
            } else {
                mindwtr::Project p;
                if (mindwtr::summarize(v, p) && !p.deleted) projects.push_back(std::move(p));
            }
        });
    static char buf[1024];
    for (;;) {
        const int n = f.read((uint8_t*)buf, sizeof buf);
        if (n <= 0) break;
        scanner.feed(buf, (size_t)n);
    }
    f.close();
    if (!scanner.finish()) {
        err = "The file is not a Mindwtr data file (or it is encrypted: end-to-end encrypted sync is not supported).";
        return false;
    }
    trimDone(tasks);
    tasks.shrink_to_fit();
    tasksArray = scanner.array("tasks");
    if (!tasksArray.found) {
        err = "The file has no task list; is this Mindwtr's data.json?";
        return false;
    }
    return true;
}

bool TasksStore::load(std::string& err) {
    pending_.clear();
    etag_.clear();
    hasData_ = false;
    tasks_.clear();
    projects_.clear();
    hal::SdSession session;
    ready_ = session.ok() && hal::sdMakeDirs(kDir);
    if (!ready_) {
        err = kNoCard;
        return false;
    }
    std::string text;
    // edits saved by a test build that kept them in internal flash
    if (storage::readFile("/tasks/pending.json", text)) {
        if (sd().exists(kPending) || writeText(kPending, text)) storage::remove("/tasks/pending.json");
        for (const char* old : {"/tasks/data.json", "/tasks/state", "/tasks/upload.tmp", "/tasks/download.tmp"})
            storage::remove(old);
    }
    if (readText(kPending, text)) mindwtr::parsePatches(text, pending_);
    if (readText(kState, text)) {
        auto lines = storage::splitLines(text);
        if (!lines.empty()) etag_ = lines[0];
    }
    bool ok = true;
    if (sd().exists(kBase)) {
        ok = index(kBase, tasks_, projects_, tasksArray_, err);
        hasData_ = ok;
        if (!ok) etag_.clear();  // download it again
    }
    applyPending();
    return ok;
}

bool TasksStore::readBase(const mindwtr::TaskRow& t, json::Value& out) const {
    if (t.end <= t.start || !hasData_) return false;
    hal::SdSession session;
    if (!session.ok()) return false;
    File f = sd().open(kBase, "r");
    if (!f || !f.seek(t.start)) return false;
    std::string text(t.end - t.start, '\0');
    const size_t n = f.read((uint8_t*)&text[0], text.size());
    f.close();
    return n == text.size() && json::parse(text, out);
}

mindwtr::Patch* TasksStore::pendingFor(const std::string& id) {
    for (auto& p : pending_)
        if (p.id == id) return &p;
    return nullptr;
}

const mindwtr::Patch* TasksStore::pendingFor(const std::string& id) const {
    for (auto& p : pending_)
        if (p.id == id) return &p;
    return nullptr;
}

// Brings the summaries up to date with the pending edits.
void TasksStore::applyPending() {
    tasks_.reserve(tasks_.size() + pending_.size());
    for (auto& p : pending_) {
        auto it = std::find_if(tasks_.begin(), tasks_.end(), [&](const mindwtr::TaskRow& t) { return t.id == p.id; });
        json::Value v;
        if (it != tasks_.end()) {
            if (it->end > it->start && !readBase(*it, v)) continue;
        } else if (!p.isNew) {
            continue;  // deleted on the server: the edit is dropped at the next upload
        }
        mindwtr::applyFields(v, p);
        mindwtr::TaskRow t;
        if (!mindwtr::summarize(v, t)) continue;
        if (it != tasks_.end()) {
            t.start = it->start;
            t.end = it->end;
            *it = std::move(t);  // a deleted task is kept (hidden) until it is uploaded
        } else {
            tasks_.push_back(std::move(t));
        }
    }
}

const mindwtr::TaskRow* TasksStore::find(const std::string& id) const {
    for (auto& t : tasks_)
        if (t.id == id) return &t;
    return nullptr;
}

std::string TasksStore::projectTitle(const std::string& id) const {
    for (auto& p : projects_)
        if (p.id == id) return p.title;
    return std::string();
}

bool TasksStore::current(const std::string& id, json::Value& out) const {
    const mindwtr::TaskRow* t = find(id);
    if (!t) return false;
    out = json::Value::object();
    if (t->end > t->start && !readBase(*t, out)) return false;
    if (const mindwtr::Patch* p = pendingFor(id)) mindwtr::applyFields(out, *p);
    return true;
}

bool TasksStore::edit(const std::string& id, const std::string& nowIso,
                      const std::function<void(mindwtr::Patch&, const json::Value&)>& fn) {
    json::Value cur;
    if (!current(id, cur)) return false;
    mindwtr::Patch* p = pendingFor(id);
    if (!p) {
        pending_.emplace_back();
        p = &pending_.back();
        p->id = id;
    }
    fn(*p, cur);
    p->setField("updatedAt", json::Value::string(nowIso));
    // refresh this task's summary
    json::Value updated = cur;
    mindwtr::applyFields(updated, *p);
    mindwtr::TaskRow s;
    auto it = std::find_if(tasks_.begin(), tasks_.end(), [&](const mindwtr::TaskRow& x) { return x.id == id; });
    if (it != tasks_.end() && mindwtr::summarize(updated, s)) {
        s.start = it->start;
        s.end = it->end;
        *it = std::move(s);  // a deleted task is kept (hidden) until it is uploaded
    }
    return savePending();
}

bool TasksStore::add(const mindwtr::Patch& p) {
    pending_.push_back(p);
    json::Value v;
    mindwtr::applyFields(v, p);
    mindwtr::TaskRow t;
    if (mindwtr::summarize(v, t)) tasks_.push_back(std::move(t));
    return savePending();
}

bool TasksStore::savePending() {
    hal::SdSession session;
    if (!session.ok()) return false;
    if (pending_.empty()) return !sd().exists(kPending) || sd().remove(kPending);
    return writeText(kPending, mindwtr::serializePatches(pending_));
}

bool TasksStore::saveState() {
    hal::SdSession session;
    return session.ok() && writeText(kState, etag_ + "\n");
}

bool TasksStore::adoptDownload(const char* path, const std::string& etag, std::string& err) {
    hal::SdSession session;
    if (!session.ok()) {
        err = kNoCard;
        return false;
    }
    // Free the current list first: holding the old and the new one at the same time
    // (next to the TLS connection) can run out of memory with a large library.
    std::vector<mindwtr::TaskRow>().swap(tasks_);
    std::vector<mindwtr::Project>().swap(projects_);
    json::ElementScanner::ArrayInfo arr;
    if (!index(path, tasks_, projects_, arr, err)) {
        sd().remove(path);
        // back to the copy we had
        std::string ignored;
        if (!hasData_ || !index(kBase, tasks_, projects_, tasksArray_, ignored)) {
            hasData_ = false;
            tasks_.clear();
            projects_.clear();
        }
        applyPending();
        return false;
    }
    if (sd().exists(kBase)) sd().remove(kBase);
    if (!sd().rename(path, kBase)) {
        err = "Could not write to the SD card (full?).";
        hasData_ = false;
        tasks_.clear();
        projects_.clear();
        applyPending();
        return false;
    }
    tasksArray_ = arr;
    hasData_ = true;
    etag_ = etag;
    saveState();
    applyPending();
    return true;
}

bool TasksStore::buildUpload(const char* path, const std::string& deviceId, size_t& size, std::string& err) {
    if (!hasData_) {
        err = "No server copy to update yet.";
        return false;
    }
    std::vector<json::Splice> splices;
    std::string added;
    for (auto& p : pending_) {
        const mindwtr::TaskRow* t = find(p.id);
        json::Value v;
        if (t && t->end > t->start) {
            if (!readBase(*t, v)) {
                err = "Could not read the local task file.";
                return false;
            }
            mindwtr::applyForUpload(v, p, deviceId);
            splices.push_back({t->start, t->end, json::serialize(v)});
        } else if (p.isNew || t) {
            mindwtr::Patch np = p;
            np.isNew = true;
            mindwtr::applyForUpload(v, np, deviceId);
            if (!added.empty()) added.push_back(',');
            json::serialize(v, added);
        }
        // otherwise: edited here, but deleted elsewhere in the meantime; the edit is dropped
    }
    if (!added.empty()) {
        if (!tasksArray_.empty) added.insert(added.begin(), ',');
        splices.push_back({tasksArray_.close, tasksArray_.close, added});
    }
    hal::SdSession session;
    if (!session.ok()) {
        err = kNoCard;
        return false;
    }
    File src = sd().open(kBase, "r");
    File dst = sd().open(path, "w");
    if (!src || !dst) {
        err = "Could not write to the SD card (full?).";
        return false;
    }
    const size_t srcSize = src.size();
    size = json::splicedSize(srcSize, splices);
    bool writeOk = true;
    // Counted here: File::size() of a file being written does not include data
    // still in the write buffer.
    size_t read = 0, written = 0;
    {
        json::Splicer splicer(std::move(splices), [&](const char* d, size_t n) {
            if (!writeOk) return;
            const size_t w = dst.write((const uint8_t*)d, n);
            written += w;
            if (w != n) writeOk = false;
        });
        static char buf[1024];
        for (;;) {
            const int n = src.read((uint8_t*)buf, sizeof buf);
            if (n <= 0) break;
            read += (size_t)n;
            splicer.feed(buf, (size_t)n);
        }
        splicer.finish();
    }
    src.close();
    dst.flush();
    dst.close();
    if (read != srcSize) {
        sd().remove(path);
        err = "Could not read the task file on the SD card.";
        return false;
    }
    if (!writeOk || written != size) {
        sd().remove(path);
        err = "Could not write to the SD card (full?).";
        return false;
    }
    return true;
}

bool TasksStore::uploaded(const char* path, const std::string& etag, std::string& err) {
    pending_.clear();
    savePending();
    return adoptDownload(path, etag, err);
}

void TasksStore::forgetServerCopy() {
    {
        hal::SdSession session;
        if (session.ok() && sd().exists(kBase)) sd().remove(kBase);
    }
    etag_.clear();
    saveState();
    hasData_ = false;
    // keep the summaries of new tasks only
    tasks_.clear();
    projects_.clear();
    applyPending();
}
