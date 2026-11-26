// Local copy of the Mindwtr sync file and the edits made on the device.
//
// The last downloaded data.json is kept as it is on the microSD card
// (/cardputerOS/tasks/data.json) and never loaded into memory as a whole: only a short
// summary of each task is. Edits are recorded as field-level patches
// (pending.json next to it) and replayed on top of the newest server copy when
// uploading, so everything else in the file (projects, settings, fields this
// app does not know) is passed through untouched.
//
// To bound memory use, archived tasks are not listed and only the most recent
// done tasks are (see kMaxDone in the .cpp).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "mw_json.h"
#include "mindwtr.h"

class TasksStore {
public:
    static const char* kDir;  // folder on the microSD card holding everything

    // Reads the cached file and the pending edits. False (with err) if the cached
    // file is unreadable; there may simply be none yet (hasData() false).
    bool load(std::string& err);
    bool hasData() const { return hasData_; }
    // load() found the SD card (false: no card, nothing loaded; call load() again).
    bool ready() const { return ready_; }

    const std::vector<mindwtr::TaskRow>& tasks() const { return tasks_; }
    const std::vector<mindwtr::Project>& projects() const { return projects_; }
    const mindwtr::TaskRow* find(const std::string& id) const;
    std::string projectTitle(const std::string& id) const;
    int pendingCount() const { return (int)pending_.size(); }

    // The task as it is now (server copy plus pending edits).
    bool current(const std::string& id, json::Value& out) const;
    // Records an edit: `fn` adds field changes to the patch, given the current task.
    // updatedAt is set automatically.
    bool edit(const std::string& id, const std::string& nowIso,
              const std::function<void(mindwtr::Patch&, const json::Value&)>& fn);
    bool add(const mindwtr::Patch& p);

    // --- for sync
    std::string etag() const { return etag_; }
    // Checks that `path` is a Mindwtr data file and makes it the new server copy.
    bool adoptDownload(const char* path, const std::string& etag, std::string& err);
    // Writes the server copy with all pending edits applied to `path`.
    bool buildUpload(const char* path, const std::string& deviceId, size_t& size, std::string& err);
    // `path` (made by buildUpload) was stored on the server: it becomes the server
    // copy and the pending edits are done.
    bool uploaded(const char* path, const std::string& etag, std::string& err);
    // Forget the server copy (the account changed). Pending edits are kept.
    void forgetServerCopy();

private:
    bool index(const char* path, std::vector<mindwtr::TaskRow>& tasks, std::vector<mindwtr::Project>& projects,
               json::ElementScanner::ArrayInfo& tasksArray, std::string& err) const;
    void applyPending();
    bool readBase(const mindwtr::TaskRow& t, json::Value& out) const;
    mindwtr::Patch* pendingFor(const std::string& id);
    const mindwtr::Patch* pendingFor(const std::string& id) const;
    bool savePending();
    bool saveState();

    std::vector<mindwtr::TaskRow> tasks_;
    std::vector<mindwtr::Project> projects_;
    std::vector<mindwtr::Patch> pending_;
    json::ElementScanner::ArrayInfo tasksArray_;
    std::string etag_;
    bool hasData_ = false;
    bool ready_ = false;
};
