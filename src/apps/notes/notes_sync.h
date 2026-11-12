// Two-way sync between the local notes and a WebDAV folder of Markdown (.md) files.
#pragma once
#include <functional>
#include <string>

#include "dav_client.h"
#include "notes_store.h"

struct SyncResult {
    bool ok = false;
    bool folderMissing = false;  // the folder does not exist (createFolder can make it)
    std::string error;
    int uploaded = 0, downloaded = 0, deleted = 0, conflicts = 0;
};

struct SyncAccount {
    std::string url, user, pass;  // url = the WebDAV folder holding the notes
    bool configured() const { return !url.empty(); }
    static SyncAccount load();  // empty if only an old CalDAV account is stored
    void save() const;
    static bool hasLegacyAccount();  // a CalDAV account from an earlier version is stored
    static void clearLegacy();
};

SyncResult syncNotes(NotesStore& store, const SyncAccount& account, const DavClient::CertFn& onCert,
                     const std::function<void(const std::string&)>& progress);

// Creates the account's folder on the server (MKCOL).
bool createFolder(const SyncAccount& account, const DavClient::CertFn& onCert, std::string& err);
