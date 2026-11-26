#include "tasks_store.h"

#include <LittleFS.h>

#include <algorithm>

#include "../../os/storage.h"

namespace {
const char* kDir = "/tasks";
const char* kBase = "/tasks/data.json";
const char* kPending = "/tasks/pending.json";
const char* kState = "/tasks/state";
constexpr size_t kMaxDone = 200;  // done tasks listed (the most recently completed)
}  // namespace

bool TasksStore::index(const char* path, std::vector<mindwtr::TaskRow>& tasks, std::vector<mindwtr::Project>& projects,
                       json::ElementScanner::ArrayInfo& tasksArray, std::string& err) const {
    tasks.clear();
    projects.clear();
    File f = LittleFS.open(path, "r");
    if (!f) {
        err = "Could not open the task file.";
        return false;
    }
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
    // keep only the most recently completed done tasks
    std::vector<int64_t> doneKeys;
    for (auto& t : tasks)
        if (t.status == "done" && !pendingFor(t.id)) doneKeys.push_back(t.completedKey);
    if (doneKeys.size() > kMaxDone) {
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
    tasks.shrink_to_fit();
    tasksArray = scanner.array("tasks");
    if (!tasksArray.found) {
        err = "The file has no task list; is this Mindwtr's data.json?";
        return false;
    }
    return true;
}

bool TasksStore::load(std::string& err) {
    storage::makeDir(kDir);
    pending_.clear();
    std::string text;
    if (storage::readFile(kPending, text)) mindwtr::parsePatches(text, pending_);
    etag_.clear();
    if (storage::readFile(kState, text)) {
        auto lines = storage::splitLines(text);
        if (!lines.empty()) etag_ = lines[0];
    }
    hasData_ = false;
    tasks_.clear();
    projects_.clear();
    bool ok = true;
    if (storage::exists(kBase)) {
        ok = index(kBase, tasks_, projects_, tasksArray_, err);
        hasData_ = ok;
        if (!ok) etag_.clear();  // download it again
    }
    applyPending();
    return ok;
}

bool TasksStore::readBase(const mindwtr::TaskRow& t, json::Value& out) const {
    if (t.end <= t.start || !hasData_) return false;
    File f = LittleFS.open(kBase, "r");
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
    if (pending_.empty()) return !storage::exists(kPending) || storage::remove(kPending);
    return storage::writeFile(kPending, mindwtr::serializePatches(pending_));
}

bool TasksStore::saveState() { return storage::writeFile(kState, etag_ + "\n"); }

bool TasksStore::adoptDownload(const char* path, const std::string& etag, std::string& err) {
    std::vector<mindwtr::TaskRow> tasks;
    std::vector<mindwtr::Project> projects;
    json::ElementScanner::ArrayInfo arr;
    if (!index(path, tasks, projects, arr, err)) {
        LittleFS.remove(path);
        return false;
    }
    LittleFS.remove(kBase);
    if (!LittleFS.rename(path, kBase)) {
        err = "Could not write to local storage (full?).";
        hasData_ = false;
        tasks_.clear();
        return false;
    }
    tasks_.swap(tasks);
    projects_.swap(projects);
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
    File src = LittleFS.open(kBase, "r");
    File dst = LittleFS.open(path, "w");
    if (!src || !dst) {
        err = "Could not write to local storage (full?).";
        return false;
    }
    size = json::splicedSize(src.size(), splices);
    bool writeOk = true;
    {
        json::Splicer splicer(std::move(splices), [&](const char* d, size_t n) {
            if (writeOk && dst.write((const uint8_t*)d, n) != n) writeOk = false;
        });
        static char buf[1024];
        for (;;) {
            const int n = src.read((uint8_t*)buf, sizeof buf);
            if (n <= 0) break;
            splicer.feed(buf, (size_t)n);
        }
        splicer.finish();
    }
    src.close();
    const size_t written = dst.size();
    dst.close();
    if (!writeOk || written != size) {
        LittleFS.remove(path);
        err = "Could not write to local storage (full?).";
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
    LittleFS.remove(kBase);
    etag_.clear();
    saveState();
    hasData_ = false;
    // keep the summaries of new tasks only
    tasks_.clear();
    projects_.clear();
    applyPending();
}
