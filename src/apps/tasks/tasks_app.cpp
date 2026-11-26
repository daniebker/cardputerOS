// Tasks: the tasks of a Mindwtr (mindwtr.app) library, read from and written
// back to the data.json file Mindwtr keeps in a WebDAV folder.
#include <Arduino.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "../../os/icons.h"
#include "../../os/kernel.h"
#include "../../os/net.h"
#include "../../os/storage.h"
#include "../../os/theme.h"
#include "../../os/ui.h"
#include "mindwtr.h"
#include "tasks_store.h"
#include "tasks_sync.h"

namespace {

namespace dr = theme::dracula;

enum Filter { Focus, Inbox, Next, Waiting, Someday, AllOpen, DoneF, kFilterCount };
const char* const kFilterNames[] = {"Focus", "Inbox", "Next", "Waiting", "Someday", "All", "Done"};
const char* const kStatusNames[] = {"Inbox", "Next", "Waiting", "Someday", "Reference", "Done", "Archived"};
constexpr uint32_t kAutoUploadMs = 4000;  // upload this long after the last edit

bool clockValid() { return time(nullptr) > 1700000000; }

mindwtr::Date today() {
    const time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    return {tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday};
}

std::string nowIso() { return mindwtr::isoTimestamp((int64_t)time(nullptr)); }

std::string newId() {
    uint8_t b[16];
    for (int i = 0; i < 16; i += 4) {
        const uint32_t r = esp_random();
        memcpy(b + i, &r, 4);
    }
    return mindwtr::uuidFromBytes(b);
}

std::string deviceId() {
    std::string id = storage::getStr("mw_dev");
    if (id.empty()) {
        id = newId();
        storage::putStr("mw_dev", id);
    }
    return id;
}

std::string statusName(const std::string& s) {
    for (int i = 0; i < mindwtr::kStatusCount; i++)
        if (s == mindwtr::kStatuses[i]) return kStatusNames[i];
    return s;
}

std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (auto& s : v) out += (out.empty() ? "" : " ") + s;
    return out;
}

// "@home, work" -> {"@home", "@work"}
std::vector<std::string> splitWords(const std::string& text, char prefix) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        if (cur.empty() || cur == std::string(1, prefix)) {
            cur.clear();
            return;
        }
        if (cur[0] != prefix) cur.insert(cur.begin(), prefix);
        if (std::find(out.begin(), out.end(), cur) == out.end()) out.push_back(cur);
        cur.clear();
    };
    for (char c : text) {
        if (c == ' ' || c == ',' || c == '\t') flush();
        else cur.push_back(c);
    }
    flush();
    return out;
}

json::Value stringArray(const std::vector<std::string>& v) {
    json::Value a = json::Value::array();
    for (auto& s : v) a.arr.push_back(json::Value::string(s));
    return a;
}

// Mindwtr date -> what the date prompt shows ("2026-11-30 14:30")
std::string dateForInput(const std::string& d) {
    mindwtr::Date day;
    if (!mindwtr::parseDay(d, day)) return d;
    std::string out = d.substr(0, 10);
    const std::string t = mindwtr::timeOf(d);
    if (!t.empty()) out += " " + t;
    return out;
}

std::vector<std::string> wrapText(os::Gfx& g, const std::string& text, int maxW, int maxLines) {
    std::vector<std::string> lines;
    std::string line, word;
    auto push = [&](const std::string& l) {
        if ((int)lines.size() < maxLines) lines.push_back(l);
    };
    size_t i = 0;
    while (i <= text.size()) {
        const size_t sp = std::min(text.find(' ', i), text.size());
        word = text.substr(i, sp - i);
        i = sp + 1;
        const std::string cand = line.empty() ? word : line + " " + word;
        if (g.textWidth(cand.c_str()) <= maxW) {
            line = cand;
            continue;
        }
        if (!line.empty()) push(line);
        line = word;
        while (line.size() > 1 && g.textWidth(line.c_str()) > maxW) {
            size_t n = line.size() - 1;
            while (n > 1 && g.textWidth(line.substr(0, n).c_str()) > maxW) n--;
            push(line.substr(0, n));
            line.erase(0, n);
        }
    }
    if (!line.empty()) push(line);
    if ((int)lines.size() == maxLines && maxLines > 0) {
        // mark that there is more
        std::string& last = lines.back();
        if (g.textWidth(text.c_str()) > maxW * maxLines) {
            while (!last.empty() && g.textWidth((last + "..").c_str()) > maxW) last.pop_back();
            last += "..";
        }
    }
    return lines;
}

class TasksApp : public os::App {
public:
    void onStart() override {
        std::string err;
        if (!store_.load(err)) loadError_ = err;
        account_ = TasksAccount::load();
        device_ = deviceId();
        rebuild();
    }

    void onResume() override {
        redraw();
        if (!loadError_.empty()) {
            ui::alert("Tasks", loadError_ + "\nIt will be downloaded again at the next sync.");
            loadError_.clear();
            redraw();
        }
        maybeAutoSync();
    }

    void onPause() override {
        if (store_.pendingCount() && account_.configured() && net::connected()) sync(false, false);
    }

    void onTick() override {
        maybeAutoSync();
        if (changedAt_ && millis() - changedAt_ > kAutoUploadMs) {
            changedAt_ = 0;
            if (store_.pendingCount() && account_.configured() && net::connected()) sync(false, true);
        }
    }

    const char* statusText() override {
        status_ = kFilterNames[filter_];
        status_ += "  " + std::to_string(visible_.size());
        if (store_.pendingCount()) status_ += "  (" + std::to_string(store_.pendingCount()) + " not synced)";
        return status_.c_str();
    }

    void onKey(const key::Event& ev) override {
        if (ev.opt()) {
            if (ev.code == 'a') setupAccount();
            else if (ev.code == 's') sync(true, true);
            return;
        }
        if (detailId_.empty()) listKey(ev);
        else detailKey(ev);
    }

private:
    // ------------------------------------------------------------ data

    // The whole task as it is now (read from storage).
    bool load(const std::string& id, mindwtr::Task& out) const {
        json::Value v;
        return store_.current(id, v) && mindwtr::summarize(v, out) && !out.deleted;
    }

    bool matches(const mindwtr::TaskRow& t, const mindwtr::Date& day) const {
        if (t.deleted) return false;
        const std::string& s = t.status;
        long d = 0;
        const bool due = mindwtr::daysUntil(t.dueDate, day, d);
        switch (filter_) {
            case Focus:
                return !mindwtr::finished(s) && s != "reference" && s != "someday" && (s == "next" || (due && d <= 0));
            case Inbox: return s == "inbox";
            case Next: return s == "next";
            case Waiting: return s == "waiting";
            case Someday: return s == "someday";
            case AllOpen: return s == "inbox" || s == "next" || s == "waiting" || s == "someday";
            default: return s == "done";
        }
    }

    // Recomputes the visible tasks (keeping the selected one selected).
    void rebuild() {
        const std::string keep = selectedId();
        const mindwtr::Date day = today();
        const auto& all = store_.tasks();
        visible_.clear();
        for (size_t i = 0; i < all.size(); i++)
            if (matches(all[i], day)) visible_.push_back((int)i);
        if (filter_ == DoneF) {
            std::stable_sort(visible_.begin(), visible_.end(),
                             [&](int a, int b) { return all[a].completedKey > all[b].completedKey; });
        } else {
            // dated tasks first, soonest first; the others in Mindwtr's order
            std::stable_sort(visible_.begin(), visible_.end(), [&](int a, int b) {
                const bool da = !all[a].dueDate.empty(), db = !all[b].dueDate.empty();
                if (da != db) return da;
                return da && all[a].dueDate < all[b].dueDate;
            });
        }
        std::vector<ui::ListItem> items;
        int sel = list_.selected;
        for (size_t k = 0; k < visible_.size(); k++) {
            const mindwtr::TaskRow& t = all[visible_[k]];
            if (t.id == keep) sel = (int)k;
            ui::ListItem it;
            const bool done = mindwtr::finished(t.status);
            it.label = std::string(done ? "[x] " : "[ ] ") + ui::displayable(t.title.empty() ? "(untitled)" : t.title);
            long d = 0;
            if (!t.dueDate.empty() && mindwtr::daysUntil(t.dueDate, day, d)) {
                it.detail = mindwtr::dateLabel(t.dueDate, day);
                if (!done && d < 0) it.color = dr::red;
                else if (!done && d == 0) it.color = dr::orange;
            } else if (filter_ == Focus || filter_ == AllOpen) {
                it.detail = statusName(t.status);
            }
            if (done) it.color = theme::dim;
            items.push_back(std::move(it));
        }
        list_.items.swap(items);
        list_.selected = sel;
        list_.clampSelection();
        if (!account_.configured() && store_.tasks().empty()) list_.emptyText = "Not set up: press opt+a";
        else if (!store_.hasData() && store_.tasks().empty()) list_.emptyText = "Not downloaded yet (opt+s to sync)";
        else list_.emptyText = "No tasks here";
    }

    std::string selectedId() const {
        if (list_.selected < 0 || list_.selected >= (int)visible_.size()) return std::string();
        const auto& all = store_.tasks();
        const int i = visible_[list_.selected];
        return i < (int)all.size() ? all[i].id : std::string();
    }

    // ------------------------------------------------------------ list screen

    void listKey(const key::Event& ev) {
        os::Gfx& g = os::gfx();
        if (ev.is(key::Tab) || ev.is(key::Right) || (ev.code == key::Tab && ev.shift()) || ev.is(key::Left)) {
            const bool back = ev.is(key::Left) || ev.shift();
            filter_ = (filter_ + (back ? kFilterCount - 1 : 1)) % kFilterCount;
            list_.selected = 0;
            rebuild();
            os::refreshStatus();
            redraw();
            return;
        }
        if (ev.isChar() && ev.code >= '1' && ev.code < '1' + kFilterCount) {
            filter_ = ev.code - '1';
            list_.selected = 0;
            rebuild();
            os::refreshStatus();
            redraw();
            return;
        }
        if (ev.is('n') || ev.is('a')) {
            addTask();
            return;
        }
        if (ev.is('r')) {
            sync(true, true);
            return;
        }
        const std::string id = selectedId();
        if (!id.empty()) {
            if (ev.is(key::Enter)) {
                openDetail(id);
                return;
            }
            if (ev.is(' ') || ev.is('x')) return toggleDone(id);
            if (ev.is('e')) return editTitle(id);
            if (ev.is('d')) return editDate(id, "dueDate", "Due date");
            if (ev.is('s')) return editStatus(id);
            if (ev.is(key::Delete)) return deleteTask(id);
        }
        if (list_.onKey(g, listRect_, ev) == ui::List::Moved) os::refreshStatus();
    }

    // ------------------------------------------------------------ detail screen

    enum Field { FTitle, FStatus, FDue, FStart, FProject, FContexts, FTags, FNotes, FChecklist, FDone, FDelete };

    void openDetail(const std::string& id) {
        detailId_ = id;
        detail_.selected = 0;
        redraw();
    }

    void closeDetail() {
        detailId_.clear();
        rebuild();
        redraw();
    }

    void buildDetail(const mindwtr::Task& t) {
        const mindwtr::Date day = today();
        fields_.clear();
        std::vector<ui::ListItem> items;
        auto add = [&](Field f, const std::string& label, const std::string& detail, uint32_t color = 0) {
            fields_.push_back(f);
            items.push_back({label, detail, color});
        };
        add(FTitle, "Title", "edit");
        add(FStatus, "Status", statusName(t.status));
        uint32_t dueColor = 0;
        long d = 0;
        if (!mindwtr::finished(t.status) && mindwtr::daysUntil(t.dueDate, day, d) && d <= 0)
            dueColor = d < 0 ? dr::red : dr::orange;
        add(FDue, "Due", t.dueDate.empty() ? "-" : mindwtr::dateLabel(t.dueDate, day), dueColor);
        add(FStart, "Start", t.startTime.empty() ? "-" : mindwtr::dateLabel(t.startTime, day));
        const std::string project = t.projectId.empty() ? "-" : store_.projectTitle(t.projectId);
        add(FProject, "Project", project.empty() ? "?" : ui::displayable(project));
        add(FContexts, "Contexts", t.contexts.empty() ? "-" : ui::displayable(joined(t.contexts)));
        add(FTags, "Tags", t.tags.empty() ? "-" : ui::displayable(joined(t.tags)));
        std::string note = t.description.substr(0, t.description.find('\n'));
        add(FNotes, "Notes", note.empty() ? "-" : ui::displayable(note));
        if (t.checklistTotal)
            add(FChecklist, "Checklist", std::to_string(t.checklistDone) + " / " + std::to_string(t.checklistTotal));
        if (mindwtr::finished(t.status)) add(FDone, "Reopen", "", dr::cyan);
        else add(FDone, "Mark done", "", dr::green);
        add(FDelete, "Delete", "", dr::red);
        detail_.items.swap(items);
        detail_.clampSelection();
    }

    void detailKey(const key::Event& ev) {
        const mindwtr::TaskRow* t = store_.find(detailId_);
        if (!t || t->deleted) {
            closeDetail();
            return;
        }
        const std::string id = detailId_;
        if (ev.code == key::Esc || ev.is(key::Backspace) || ev.is(key::Left)) {
            closeDetail();
            return;
        }
        if (ev.is(' ') || ev.is('x')) return toggleDone(id);
        if (ev.is('e')) return editTitle(id);
        if (ev.is('d')) return editDate(id, "dueDate", "Due date");
        if (ev.is('s')) return editStatus(id);
        if (ev.is(key::Delete)) return deleteTask(id);
        os::Gfx& g = os::gfx();
        if (detail_.onKey(g, detailRect_, ev) != ui::List::Activated) return;
        if (detail_.selected < 0 || detail_.selected >= (int)fields_.size()) return;
        switch (fields_[detail_.selected]) {
            case FTitle: editTitle(id); break;
            case FStatus: editStatus(id); break;
            case FDue: editDate(id, "dueDate", "Due date"); break;
            case FStart: editDate(id, "startTime", "Start date"); break;
            case FProject: editProject(id); break;
            case FContexts: editWords(id, "contexts", "Contexts", '@'); break;
            case FTags: editWords(id, "tags", "Tags", '#'); break;
            case FNotes: editNotes(id); break;
            case FChecklist: editChecklist(id); break;
            case FDone: toggleDone(id); break;
            case FDelete: deleteTask(id); break;
        }
    }

    // ------------------------------------------------------------ editing

    bool canEdit() {
        if (clockValid()) return true;
        ui::alert("Tasks", "The clock is not set yet. Connect to Wi-Fi once so changes get the right time.");
        redraw();
        return false;
    }

    // Records an edit and schedules the upload.
    void apply(const std::string& id, const std::function<void(mindwtr::Patch&, const json::Value&)>& fn) {
        if (!store_.edit(id, nowIso(), fn)) ui::alert("Tasks", "Could not save the change (storage full?).");
        changed();
    }

    void changed() {
        changedAt_ = millis();
        if (!changedAt_) changedAt_ = 1;
        rebuild();
        os::refreshStatus();
        redraw();
    }

    void toggleDone(const std::string& id) {
        mindwtr::Task task;
        if (!load(id, task) || !canEdit()) return;
        const mindwtr::Task* t = &task;
        const bool reopen = mindwtr::finished(t->status);
        if (!reopen && t->recurring &&
            !ui::confirm("Repeating task", "This task repeats. Mindwtr creates the next one when you complete it "
                                           "there; marking it done here does not.\nMark it done anyway?")) {
            redraw();
            return;
        }
        const std::string now = nowIso();
        apply(id, [&](mindwtr::Patch& p, const json::Value& cur) {
            mindwtr::changeStatus(p, cur, reopen ? "next" : "done", now);
        });
    }

    void editStatus(const std::string& id) {
        mindwtr::Task task;
        if (!load(id, task) || !canEdit()) return;
        const mindwtr::Task* t = &task;
        std::vector<std::string> names(kStatusNames, kStatusNames + mindwtr::kStatusCount);
        int initial = 0;
        for (int i = 0; i < mindwtr::kStatusCount; i++)
            if (t->status == mindwtr::kStatuses[i]) initial = i;
        const int pick = ui::choose("Status", names, initial);
        if (pick < 0 || pick == initial) {
            redraw();
            return;
        }
        const std::string status = mindwtr::kStatuses[pick], now = nowIso();
        apply(id, [&](mindwtr::Patch& p, const json::Value& cur) { mindwtr::changeStatus(p, cur, status, now); });
    }

    void editTitle(const std::string& id) {
        mindwtr::Task task;
        if (!load(id, task) || !canEdit()) return;
        const mindwtr::Task* t = &task;
        std::string title = t->title;
        if (!ui::prompt("Title", title) || title == t->title || title.find_first_not_of(' ') == std::string::npos) {
            redraw();
            return;
        }
        apply(id, [&](mindwtr::Patch& p, const json::Value&) { p.setField("title", json::Value::string(title)); });
    }

    void editDate(const std::string& id, const char* field, const char* title) {
        mindwtr::Task task;
        if (!load(id, task) || !canEdit()) return;
        const mindwtr::Task* t = &task;
        const std::string before = field[0] == 'd' ? t->dueDate : t->startTime;
        const std::string shown = dateForInput(before);
        std::string text = shown;
        for (;;) {
            if (!ui::prompt(title, text, false, "today tom fri +3 +2w 12-24 2026-12-24, opt. 14:30   empty: none   ctrl+u: clear")) {
                redraw();
                return;
            }
            std::string value;
            if (mindwtr::parseDateInput(text, today(), value)) {
                if (value == before || text == shown) {
                    redraw();
                    return;
                }
                const std::string key = field;
                apply(id, [&](mindwtr::Patch& p, const json::Value&) {
                    if (value.empty()) p.unsetField(key);
                    else p.setField(key, json::Value::string(value));
                });
                return;
            }
            ui::alert(title, "\"" + text + "\" is not a date I understand. Try e.g. tomorrow, fri, +3, 12-24, "
                             "2026-12-24 or 2026-12-24 14:30.");
        }
    }

    void editWords(const std::string& id, const char* field, const char* title, char prefix) {
        mindwtr::Task task;
        if (!load(id, task) || !canEdit()) return;
        const mindwtr::Task* t = &task;
        const std::vector<std::string>& old = field[0] == 'c' ? t->contexts : t->tags;
        std::string text = joined(old);
        const char* hint = prefix == '@' ? "Separated by spaces, e.g. @home @phone" : "Separated by spaces, e.g. #errand #work";
        if (!ui::prompt(title, text, false, hint)) {
            redraw();
            return;
        }
        const std::vector<std::string> words = splitWords(text, prefix);
        if (words == old) {
            redraw();
            return;
        }
        const std::string key = field;
        apply(id, [&](mindwtr::Patch& p, const json::Value&) { p.setField(key, stringArray(words)); });
    }

    void editProject(const std::string& id) {
        mindwtr::Task task;
        if (!load(id, task) || !canEdit()) return;
        const mindwtr::Task* t = &task;
        std::vector<const mindwtr::Project*> projects;
        for (auto& p : store_.projects())
            if (p.status != "archived") projects.push_back(&p);
        std::sort(projects.begin(), projects.end(),
                  [](const mindwtr::Project* a, const mindwtr::Project* b) { return a->title < b->title; });
        std::vector<std::string> names = {"(no project)"};
        int initial = 0;
        for (size_t i = 0; i < projects.size(); i++) {
            names.push_back(ui::displayable(projects[i]->title));
            if (projects[i]->id == t->projectId) initial = (int)i + 1;
        }
        const int pick = ui::choose("Project", names, initial);
        if (pick < 0 || pick == initial) {
            redraw();
            return;
        }
        const std::string pid = pick == 0 ? std::string() : projects[pick - 1]->id;
        apply(id, [&](mindwtr::Patch& p, const json::Value&) {
            if (pid.empty()) p.unsetField("projectId");
            else p.setField("projectId", json::Value::string(pid));
            p.unsetField("sectionId");  // sections belong to a project
        });
    }

    void editNotes(const std::string& id) {
        json::Value cur;
        if (!store_.current(id, cur)) return;
        std::string text = cur.getString("description");
        if (text.find('\n') != std::string::npos || text.size() > 200) {
            // the prompt is one line: show longer notes instead of editing them
            ui::alert("Notes", ui::displayable(text));
            redraw();
            return;
        }
        if (!canEdit()) return;
        const std::string before = text;
        if (!ui::prompt("Notes", text) || text == before) {
            redraw();
            return;
        }
        apply(id, [&](mindwtr::Patch& p, const json::Value&) {
            if (text.empty()) p.unsetField("description");
            else p.setField("description", json::Value::string(text));
        });
    }

    void editChecklist(const std::string& id) {
        int initial = 0;
        for (;;) {
            json::Value cur;
            if (!store_.current(id, cur)) return;
            const json::Value* list = cur.get("checklist");
            if (!list || list->type != json::Value::Array || list->arr.empty()) break;
            std::vector<std::string> names;
            for (auto& item : list->arr) {
                const json::Value* done = item.get("isCompleted");
                const bool on = done && done->type == json::Value::Bool && done->b;
                names.push_back(std::string(on ? "[x] " : "[ ] ") + ui::displayable(item.getString("title")));
            }
            const int pick = ui::choose("Checklist (Enter: tick / untick)", names, initial);
            if (pick < 0 || !canEdit()) break;
            initial = pick;
            json::Value updated = *list;
            json::Value& item = updated.arr[pick];
            const json::Value* done = item.get("isCompleted");
            item.set("isCompleted", json::Value::boolean(!(done && done->type == json::Value::Bool && done->b)));
            if (!store_.edit(id, nowIso(), [&](mindwtr::Patch& p, const json::Value&) { p.setField("checklist", updated); })) {
                ui::alert("Tasks", "Could not save the change (storage full?).");
                break;
            }
            changedAt_ = millis() | 1;
        }
        changed();
    }

    void deleteTask(const std::string& id) {
        mindwtr::Task task;
        if (!load(id, task) || !canEdit()) return;
        const mindwtr::Task* t = &task;
        std::string title = ui::displayable(t->title);
        if (title.size() > 60) title = title.substr(0, 58) + "..";
        if (!ui::confirm("Delete task", "Delete \"" + title + "\"?\nIt goes to Mindwtr's trash.")) {
            redraw();
            return;
        }
        const std::string now = nowIso();
        apply(id, [&](mindwtr::Patch& p, const json::Value&) { p.setField("deletedAt", json::Value::string(now)); });
        if (detailId_ == id) closeDetail();
    }

    void addTask() {
        if (!canEdit()) return;
        std::string text;
        if (!ui::prompt("New task", text, false, "@context and #tag words are picked up") ||
            text.find_first_not_of(' ') == std::string::npos) {
            redraw();
            return;
        }
        // @contexts and #tags typed in the title
        std::vector<std::string> contexts, tags;
        std::string title, word;
        auto flush = [&]() {
            if (word.size() > 1 && word[0] == '@') contexts.push_back(word);
            else if (word.size() > 1 && word[0] == '#') tags.push_back(word);
            else if (!word.empty()) title += (title.empty() ? "" : " ") + word;
            word.clear();
        };
        for (char c : text) {
            if (c == ' ') flush();
            else word.push_back(c);
        }
        flush();
        if (title.empty()) title = text;
        static const char* const kNewStatus[] = {"next", "inbox", "next", "waiting", "someday", "inbox", "inbox"};
        const std::string id = newId(), now = nowIso();
        mindwtr::Patch p = mindwtr::newTask(id, title, kNewStatus[filter_], now);
        if (!contexts.empty()) p.set.set("contexts", stringArray(contexts));
        if (!tags.empty()) p.set.set("tags", stringArray(tags));
        if (!store_.add(p)) ui::alert("Tasks", "Could not save the task (storage full?).");
        if (filter_ == DoneF) filter_ = Inbox;
        // select the new task
        rebuild();
        for (size_t k = 0; k < visible_.size(); k++)
            if (store_.tasks()[visible_[k]].id == id) list_.selected = (int)k;
        changed();
    }

    // ------------------------------------------------------------ sync

    void maybeAutoSync() {
        if (!autoSynced_ && account_.configured() && net::connected()) {
            autoSynced_ = true;
            sync(false, true);
        }
    }

    void setupAccount() {
        TasksAccount a = account_;
        if (!a.configured() && storage::getStr("dav_kind") == "webdav") {
            // start from the Notes server: same host, a "Mindwtr" folder
            a.url = storage::getStr("dav_url");
            a.user = storage::getStr("dav_user");
            while (!a.url.empty() && a.url.back() == '/') a.url.pop_back();
            const size_t slash = a.url.rfind('/');
            if (slash != std::string::npos && slash > 8) a.url = a.url.substr(0, slash + 1) + "Mindwtr/";
        }
        if (ui::prompt("Mindwtr WebDAV folder", a.url, false, "The folder set in Mindwtr's WebDAV sync (it holds data.json)") &&
            ui::prompt("User name", a.user) && ui::prompt("Password", a.pass, true, "Stored encrypted on this device.")) {
            dav::Url u;
            if (!a.url.empty() && !dav::parseUrl(a.url, u)) {
                ui::alert("Tasks", "The address must start with https:// or http://");
            } else {
                if (a.url != account_.url) store_.forgetServerCopy();
                account_ = a;
                account_.save();
                syncInfo_.clear();
                rebuild();
                redraw();
                if (account_.configured()) sync(true, true);
                return;
            }
        }
        redraw();
    }

    // interactive: the user asked, so explain problems. mayAsk: dialogs are allowed.
    void sync(bool interactive, bool mayAsk) {
        if (!account_.configured() || !net::connected()) {
            if (interactive) {
                ui::alert("Tasks", !account_.configured() ? "No Mindwtr folder yet. Press opt+a to set it up."
                                                           : "Wi-Fi is not connected.");
                redraw();
            }
            return;
        }
        changedAt_ = 0;
        DavClient::CertFn onCert = nullptr;
        if (mayAsk) {
            onCert = [](const std::string& host, const std::string& fp, bool changed) {
                std::string shown;
                for (size_t i = 0; i < fp.size(); i += 4) shown += fp.substr(i, 4) + " ";
                if (changed)
                    return ui::confirm("CERTIFICATE CHANGED",
                                       "The certificate of " + host + " is different from last time. That is normal after a "
                                       "renewal, but could also mean someone is intercepting the connection.\nSHA-256: " +
                                           shown + "\nTrust the new certificate?");
                return ui::confirm("New server", "First connection to " + host + ".\nCertificate SHA-256:\n" + shown +
                                                     "\nTrust this server?");
            };
        }
        auto progress = [mayAsk](const std::string& m) {
            if (mayAsk) ui::busy(m);
        };
        const TasksSyncResult r = syncTasks(store_, account_, device_, onCert, progress);
        if (r.ok) {
            const time_t t = time(nullptr);
            struct tm tmv;
            localtime_r(&t, &tmv);
            char buf[24];
            snprintf(buf, sizeof buf, "synced %02d:%02d", tmv.tm_hour, tmv.tm_min);
            syncInfo_ = buf;
        } else {
            syncInfo_ = "sync failed";
            if (interactive || mayAsk) ui::alert("Tasks sync failed", r.error);
        }
        rebuild();
        os::refreshStatus();
        if (mayAsk) redraw();
    }

    // ------------------------------------------------------------ drawing

    void redraw() {
        os::Gfx& g = os::gfx();
        const os::Rect c = os::content();
        g.fillRect(c.x, c.y, c.w, c.h, theme::bg);
        if (!detailId_.empty()) {
            mindwtr::Task t;
            if (load(detailId_, t)) return drawDetail(g, c, t);
            detailId_.clear();
        }
        std::string title = "Tasks";
        if (!account_.configured()) title += "   - not set up";
        else if (store_.pendingCount()) title += "   - " + std::to_string(store_.pendingCount()) + " not synced";
        else if (!syncInfo_.empty()) title += "   - " + syncInfo_;
        os::Rect body = ui::header(g, c, title.c_str());
        body = ui::footer(g, body, "Enter: open  spc: done  n: new  d: due  s: status  e: edit  r: sync  Tab: view");
        // view tabs
        g.setFont(theme::fontSmall());
        const int tabH = g.fontHeight() + 6;
        int x = body.x + 6;
        g.setTextDatum(textdatum_t::top_left);
        for (int i = 0; i < kFilterCount; i++) {
            const int w = g.textWidth(kFilterNames[i]) + 10;
            if (i == filter_) g.fillRoundRect(x, body.y + 2, w, tabH - 2, 3, theme::select);
            g.setTextColor(i == filter_ ? theme::accent : theme::dim);
            g.drawString(kFilterNames[i], x + 5, body.y + 4);
            x += w + 2;
        }
        listRect_ = {body.x, body.y + tabH + 2, body.w, body.h - tabH - 2};
        list_.draw(g, listRect_);
    }

    void drawDetail(os::Gfx& g, const os::Rect& c, const mindwtr::Task& t) {
        buildDetail(t);
        os::Rect body = ui::header(g, c, (statusName(t.status) + " task").c_str());
        body = ui::footer(g, body, "Enter: change  spc: done  Esc / Backspace: back");
        g.setFont(theme::fontUi());
        g.setTextColor(mindwtr::finished(t.status) ? theme::dim : theme::text);
        g.setTextDatum(textdatum_t::top_left);
        const int lineH = g.fontHeight() + 2;
        const auto lines = wrapText(g, ui::displayable(t.title.empty() ? "(untitled)" : t.title), body.w - 16, 2);
        int y = body.y + 4;
        for (auto& l : lines) {
            g.drawString(l.c_str(), body.x + 8, y);
            y += lineH;
        }
        y += 2;
        g.drawFastHLine(body.x + 6, y, body.w - 12, theme::select);
        detailRect_ = {body.x, y + 2, body.w, body.y + body.h - y - 2};
        detail_.draw(g, detailRect_);
    }

    TasksStore store_;
    TasksAccount account_;
    std::string device_, loadError_, syncInfo_, status_, detailId_;
    std::vector<int> visible_;
    std::vector<Field> fields_;
    ui::List list_, detail_;
    os::Rect listRect_{0, 0, 0, 0}, detailRect_{0, 0, 0, 0};
    int filter_ = Focus;
    uint32_t changedAt_ = 0;
    bool autoSynced_ = false;
};

OS_REGISTER_APP(TasksApp, "Tasks", "Mindwtr tasks, WebDAV sync", theme::dracula::green, icons::checklist, 21);

}  // namespace
