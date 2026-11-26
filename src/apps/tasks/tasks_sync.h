// Downloads Mindwtr's data.json from a WebDAV folder and uploads it again with
// the edits made on the device. Uploads are conditional (If-Match), so a copy
// changed by Mindwtr in the meantime is downloaded first and the edits are
// replayed on top of it.
#pragma once
#include <functional>
#include <string>

#include "../notes/dav_client.h"
#include "tasks_store.h"

struct TasksAccount {
    std::string url, user, pass;  // the WebDAV folder Mindwtr syncs to (or the data.json file itself)
    bool configured() const { return !url.empty(); }
    static TasksAccount load();
    void save() const;
    // Folder URL and file name the account points at.
    void split(std::string& folder, std::string& file) const;
};

struct TasksSyncResult {
    bool ok = false;
    std::string error;
    bool downloaded = false;  // a newer copy was downloaded
    int uploaded = 0;         // edits uploaded
};

TasksSyncResult syncTasks(TasksStore& store, const TasksAccount& account, const std::string& deviceId,
                          const DavClient::CertFn& onCert, const std::function<void(const std::string&)>& progress);
