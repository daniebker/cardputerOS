// Two-way sync of the journal's day files with a WebDAV folder. When a day was
// changed both here and on the server, the entries of both versions are merged.
#pragma once
#include <functional>
#include <string>

#include "../notes/dav_client.h"
#include "journal_store.h"

struct JournalAccount {
    std::string url, user, pass;  // url = the WebDAV folder holding the day files
    bool configured() const { return !url.empty(); }
    static JournalAccount load();
    void save() const;
};

struct JournalSyncResult {
    bool ok = false;
    std::string error;
    int uploaded = 0, downloaded = 0, merged = 0, removed = 0;
};

JournalSyncResult syncJournal(JournalStore& store, const JournalAccount& account, const DavClient::CertFn& onCert,
                              const std::function<void(const std::string&)>& progress);
