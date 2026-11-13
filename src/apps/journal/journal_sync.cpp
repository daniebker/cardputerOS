#include "journal_sync.h"

#include <map>

#include "../../os/storage.h"
#include "journal.h"

JournalAccount JournalAccount::load() {
    JournalAccount a;
    a.url = storage::getStr("jr_url");
    a.user = storage::getStr("jr_user");
    a.pass = storage::getStr("jr_pass");
    return a;
}

void JournalAccount::save() const {
    storage::putStr("jr_url", url);
    storage::putStr("jr_user", user);
    storage::putStr("jr_pass", pass);
}

namespace {

std::string httpError(const char* what, const dav::HttpResponse& r) {
    std::string msg;
    if (r.status == 401) msg = "The server rejected the user name or password.";
    else if (r.status == 403) msg = std::string(what) + " was refused (no permission).";
    else if (r.status == 507) msg = "The server is out of space.";
    else msg = std::string(what) + " failed (HTTP " + std::to_string(r.status) + ").";
    const std::string detail = dav::errorMessage(r.body);
    if (!detail.empty()) msg += "\nServer says: " + detail;
    return msg;
}

struct Sync {
    JournalStore& store;
    DavClient& client;
    JournalSyncResult result;
    const std::function<void(const std::string&)>& progress;

    Sync(JournalStore& s, DavClient& c, const std::function<void(const std::string&)>& p) : store(s), client(c), progress(p) {}

    bool fail(const std::string& msg) {
        if (result.error.empty()) result.error = msg;
        return false;
    }

    std::string href(const std::string& date) const { return client.url().path + date + ".md"; }

    // GET a day. `gone` is set if it no longer exists.
    bool fetch(const std::string& date, std::string& text, std::string& etag, bool& gone) {
        gone = false;
        dav::HttpResponse r;
        std::string err;
        if (!client.request("GET", href(date), {}, "", r, err)) return fail(err);
        if (r.status == 404 || r.status == 410) {
            gone = true;
            return true;
        }
        if (r.status != 200) return fail(httpError("Download", r));
        if (r.truncated) return fail(date + ".md is too large for this device.");
        text.clear();
        for (char c : r.body)
            if (c != '\r') text.push_back(c);
        if (!r.etag.empty()) etag = r.etag;
        return true;
    }

    // PUT a day. `conflict`: the server copy is not the one we expected (HTTP 412).
    bool put(const std::string& date, const std::string& text, const std::string& ifMatch, bool mustBeNew,
             std::string& etag, bool& conflict) {
        conflict = false;
        std::vector<DavClient::Header> h = {{"Content-Type", "text/markdown; charset=utf-8"}};
        if (mustBeNew) h.push_back({"If-None-Match", "*"});
        else if (!ifMatch.empty()) h.push_back({"If-Match", ifMatch});
        dav::HttpResponse r;
        std::string err;
        if (!client.request("PUT", href(date), h, text, r, err)) return fail(err);
        if (r.status == 412) {
            conflict = true;
            return true;
        }
        if (r.status < 200 || r.status >= 300) return fail(httpError("Upload", r));
        etag = r.etag;  // may be empty: the next sync then re-reads the file
        return true;
    }

    // The day exists on both sides in different versions: merge the entries and upload.
    bool mergeDay(JournalDay& d, std::string remoteEtag) {
        for (int attempt = 0; attempt < 3; attempt++) {
            std::string remote;
            bool gone;
            if (!fetch(d.date, remote, remoteEtag, gone)) return false;
            const std::string local = store.text(d.date);
            const std::string merged = gone ? local : journal::merge(local, remote);
            if (!store.setText(d.date, merged, false)) return fail("Could not write to local storage (full?).");
            if (!gone && merged == remote) {  // the server already has everything
                d.etag = remoteEtag;
                d.dirty = false;
                result.downloaded++;
                return true;
            }
            std::string etag;
            bool conflict;
            if (!put(d.date, merged, remoteEtag, gone, etag, conflict)) return false;
            if (!conflict) {
                d.etag = etag;
                d.dirty = false;
                result.merged++;
                return true;
            }
            // changed again while we were merging: go round once more
        }
        d.dirty = true;
        return fail("The server copy of " + d.date + " keeps changing; try again later.");
    }

    bool run() {
        progress("Reading the journal folder...");
        std::map<std::string, std::string> remote;  // date -> etag
        {
            dav::MultistatusParser parser([&](const dav::Resource& r) {
                if (r.isCollection) return;
                std::string name = dav::decodePath(dav::hrefPath(r.href));
                name = name.substr(name.rfind('/') + 1);
                if (name.size() != 13 || name.compare(10, 3, ".md") != 0) return;
                name.resize(10);
                if (journal::validDate(name)) remote[name] = r.etag;
            });
            const std::vector<DavClient::Header> hdr = {{"Depth", "1"}, {"Content-Type", "application/xml; charset=utf-8"}};
            for (int attempt = 0;; attempt++) {
                dav::HttpResponse r;
                std::string err, head;
                if (!client.request("PROPFIND", client.url().path, hdr, dav::listFolderBody(), r, err,
                                    [&](const char* d, size_t n) {
                                        if (head.size() < 2048) head.append(d, std::min(n, 2048 - head.size()));
                                        parser.feed(d, n);
                                    }))
                    return fail(err);
                r.body = head;
                if (r.status == 207) break;
                if (r.status == 404 && attempt == 0) {
                    // first sync: create the folder
                    dav::HttpResponse mk;
                    if (!client.request("MKCOL", client.url().path, {}, "", mk, err)) return fail(err);
                    if (mk.status == 201 || mk.status == 405) continue;
                    if (mk.status == 409) return fail("The journal folder's parent folder does not exist on the server.");
                    return fail(httpError("Creating the journal folder", mk));
                }
                return fail(r.status == 404 ? "Journal folder not found on the server." : httpError("Reading the folder", r));
            }
        }

        // days we have
        std::vector<std::string> dates;
        for (auto& d : store.days()) dates.push_back(d.date);
        int step = 0;
        for (auto& date : dates) {
            JournalDay* d = store.find(date);
            if (!d) continue;
            auto it = remote.find(date);
            if (it == remote.end()) {
                if (d->etag.empty() || d->dirty) {
                    progress("Uploading " + std::to_string(++step) + "...");
                    std::string etag;
                    bool conflict;
                    if (!put(date, store.text(date), "", true, etag, conflict)) return false;
                    if (conflict) {
                        if (!mergeDay(*d, "")) return false;
                    } else {
                        d->etag = etag;
                        d->dirty = false;
                        result.uploaded++;
                    }
                } else {
                    store.remove(date);  // deleted on the server
                    result.removed++;
                }
                continue;
            }
            const std::string remoteEtag = it->second;
            remote.erase(it);
            if (remoteEtag == d->etag && !d->etag.empty()) {
                if (!d->dirty) continue;
                progress("Uploading " + std::to_string(++step) + "...");
                std::string etag;
                bool conflict;
                if (!put(date, store.text(date), d->etag, false, etag, conflict)) return false;
                if (conflict) {
                    if (!mergeDay(*d, remoteEtag)) return false;
                } else {
                    d->etag = etag;
                    d->dirty = false;
                    result.uploaded++;
                }
            } else if (d->dirty || d->etag.empty()) {
                progress("Merging " + date + "...");
                if (!mergeDay(*d, remoteEtag)) return false;
            } else {
                progress("Downloading " + std::to_string(++step) + "...");
                std::string text, etag = remoteEtag;
                bool gone;
                if (!fetch(date, text, etag, gone)) return false;
                if (gone) {
                    store.remove(date);
                    result.removed++;
                } else {
                    if (!store.setText(date, text, false)) return fail("Could not write to local storage (full?).");
                    store.find(date)->etag = etag;
                    result.downloaded++;
                }
            }
        }

        // days that only exist on the server
        for (auto& kv : remote) {
            progress("Downloading " + std::to_string(++step) + "...");
            std::string text, etag = kv.second;
            bool gone;
            if (!fetch(kv.first, text, etag, gone)) return false;
            if (gone) continue;
            if (!store.setText(kv.first, text, false)) return fail("Could not write to local storage (full?).");
            store.find(kv.first)->etag = etag;
            result.downloaded++;
        }
        return true;
    }
};

}  // namespace

JournalSyncResult syncJournal(JournalStore& store, const JournalAccount& account, const DavClient::CertFn& onCert,
                              const std::function<void(const std::string&)>& progress) {
    DavClient client;
    client.onCert = onCert;
    Sync s(store, client, progress);
    std::string err;
    if (!client.open(account.url, account.user, account.pass, err)) {
        s.result.error = err;
        return s.result;
    }
    s.result.ok = s.run();
    client.close();
    store.save();  // keep whatever progress was made, even after an error
    return s.result;
}
