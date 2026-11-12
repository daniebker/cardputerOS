#include "notes_sync.h"

#include <Arduino.h>

#include <map>

#include "../../os/storage.h"

// NVS keys. "dav_kind" marks the account as WebDAV; accounts saved by the
// CalDAV version of the app do not have it.
SyncAccount SyncAccount::load() {
    SyncAccount a;
    if (storage::getStr("dav_kind") != "webdav") return a;
    a.url = storage::getStr("dav_url");
    a.user = storage::getStr("dav_user");
    a.pass = storage::getStr("dav_pass");
    return a;
}

void SyncAccount::save() const {
    storage::putStr("dav_kind", "webdav");
    storage::putStr("dav_url", url);
    storage::putStr("dav_user", user);
    storage::putStr("dav_pass", pass);
}

bool SyncAccount::hasLegacyAccount() { return storage::getStr("dav_kind") != "webdav" && !storage::getStr("dav_url").empty(); }

void SyncAccount::clearLegacy() {
    storage::putStr("dav_url", "");
    storage::putStr("dav_pass", "");
}

namespace {

constexpr int kMaxNameTries = 20;

bool endsWithMd(const std::string& path) {
    if (path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 3);
    for (char& c : ext)
        if (c >= 'A' && c <= 'Z') c += 32;
    return ext == ".md";
}

std::string httpError(const char* what, const dav::HttpResponse& r) {
    std::string msg;
    if (r.status == 401) msg = "The server rejected the user name or password.";
    else if (r.status == 403) msg = std::string(what) + " was refused (no permission).";
    else if (r.status == 404) msg = "Folder not found on the server. Check the address.";
    else if (r.status == 507) msg = "The server is out of space.";
    else msg = std::string(what) + " failed (HTTP " + std::to_string(r.status) + ").";
    std::string detail = dav::errorMessage(r.body);
    if (!detail.empty()) msg += "\nServer says: " + detail;
    return msg;
}

struct Sync {
    NotesStore& store;
    DavClient& client;
    SyncResult result;
    const std::function<void(const std::string&)>& progress;

    Sync(NotesStore& s, DavClient& c, const std::function<void(const std::string&)>& p) : store(s), client(c), progress(p) {}

    bool fail(const std::string& msg) {
        if (result.error.empty()) result.error = msg;
        return false;
    }

    // GET the file into the note. `gone` is set if it vanished or is unusable.
    bool download(Note& n, const std::string& href, const std::string& listedEtag, bool& gone) {
        gone = false;
        dav::HttpResponse r;
        std::string err;
        if (!client.request("GET", href, {}, "", r, err)) return fail(err);
        if (r.status == 404 || r.status == 410) {
            gone = true;
            return true;
        }
        if (r.status != 200) return fail(httpError("Download", r));
        if (r.truncated) {
            gone = true;  // larger than this device handles: leave it on the server untouched
            return true;
        }
        std::string text;
        text.reserve(r.body.size());
        for (char c : r.body)
            if (c != '\r') text.push_back(c);
        while (!text.empty() && text.back() == '\n') text.pop_back();
        if (NotesStore::titleOf(text).empty()) {
            // no title line: use the file name, as Markdown note apps do
            std::string name = dav::decodePath(href.substr(href.rfind('/') + 1));
            name = name.substr(0, name.size() - 3);
            text = "# " + name + (text.empty() ? "" : "\n\n" + text);
        }
        n.href = href;
        n.etag = r.etag.empty() ? listedEtag : r.etag;
        n.dirty = false;
        n.deleted = false;
        if (!store.setText(n, text, false)) return fail("Could not write to local storage (full?).");
        result.downloaded++;
        return true;
    }

    // PUT the note. `conflict` is set when the server copy changed underneath us (HTTP 412).
    bool upload(Note& n, bool& conflict) {
        conflict = false;
        std::string body = store.text(n);
        if (body.empty() || body.back() != '\n') body.push_back('\n');
        const std::vector<DavClient::Header> base = {{"Content-Type", "text/markdown; charset=utf-8"}};

        if (!n.href.empty()) {
            std::vector<DavClient::Header> h = base;
            if (!n.etag.empty()) h.push_back({"If-Match", n.etag});
            dav::HttpResponse r;
            std::string err;
            if (!client.request("PUT", n.href, h, body, r, err)) return fail(err);
            if (r.status == 412) {
                conflict = true;
                return true;
            }
            if (r.status < 200 || r.status >= 300) return fail(httpError("Upload", r));
            n.etag = r.etag;  // may be empty: the next sync then re-reads the file
            n.dirty = false;
            result.uploaded++;
            return true;
        }

        // new file: name it after the title, adding " 2", " 3"... if the name is taken
        const std::string stem = dav::fileStem(n.title);
        for (int i = 1; i <= kMaxNameTries; i++) {
            const std::string name = stem + (i > 1 ? " " + std::to_string(i) : "") + ".md";
            const std::string href = client.url().path + dav::encodeSegment(name);
            std::vector<DavClient::Header> h = base;
            h.push_back({"If-None-Match", "*"});
            dav::HttpResponse r;
            std::string err;
            if (!client.request("PUT", href, h, body, r, err)) return fail(err);
            if (r.status == 412) continue;  // exists
            if (r.status < 200 || r.status >= 300) return fail(httpError("Upload", r));
            n.href = href;
            n.etag = r.etag;
            n.dirty = false;
            result.uploaded++;
            return true;
        }
        return fail("Could not find a free file name for \"" + n.title + "\".");
    }

    // Our edit and a server edit collided: keep ours as a separate new note.
    void keepLocalAsCopy(Note& n) {
        std::string text = store.text(n);
        size_t nl = text.find('\n');
        text.insert(nl == std::string::npos ? text.size() : nl, " (conflict copy)");
        store.create(text);  // note: invalidates Note references
        result.conflicts++;
    }

    bool run() {
        // 1. what is in the folder?
        progress("Reading the folder...");
        // keyed by the decoded path: servers differ in which characters they percent-encode
        struct RemoteFile {
            std::string href, etag;
        };
        std::map<std::string, RemoteFile> remote;
        const std::string folder = client.url().path;
        {
            dav::MultistatusParser parser([&](const dav::Resource& r) {
                std::string path = dav::hrefPath(r.href);
                if (r.isCollection || path.empty() || path.back() == '/' || !endsWithMd(path)) return;
                remote[dav::decodePath(path)] = {path, r.etag};
            });
            dav::HttpResponse r;
            std::string err, head;
            if (!client.request("PROPFIND", folder, {{"Depth", "1"}, {"Content-Type", "application/xml; charset=utf-8"}},
                                dav::listFolderBody(), r, err, [&](const char* d, size_t n) {
                                    if (head.size() < 2048) head.append(d, std::min(n, 2048 - head.size()));
                                    parser.feed(d, n);
                                }))
                return fail(err);
            r.body = head;
            if (r.status == 404) {
                result.folderMissing = true;
                return fail("The folder does not exist on the server.");
            }
            if (r.status != 207) return fail(httpError("Reading the folder", r));
        }

        // 2. notes we already know about (by id: the vector may grow)
        std::vector<std::string> ids;
        for (auto& n : store.all()) ids.push_back(n.id);
        int step = 0;
        for (auto& id : ids) {
            Note* n = store.find(id);
            if (!n || n->href.empty()) continue;
            progress("Syncing " + std::to_string(++step) + "...");
            auto it = remote.find(dav::decodePath(n->href));
            if (it == remote.end()) {
                // deleted (or renamed) on the server
                if (n->dirty && !n->deleted) {
                    n->href.clear();  // our edit wins: upload again as a new file below
                    n->etag.clear();
                } else {
                    store.purge(id);
                    result.deleted++;
                }
                continue;
            }
            const std::string remoteEtag = it->second.etag;
            n->href = it->second.href;  // adopt the server's spelling of the path
            remote.erase(it);
            const bool remoteChanged = remoteEtag != n->etag;

            if (n->deleted) {
                if (!remoteChanged) {
                    dav::HttpResponse r;
                    std::string err;
                    std::vector<DavClient::Header> h;
                    if (!n->etag.empty()) h.push_back({"If-Match", n->etag});
                    if (!client.request("DELETE", n->href, h, "", r, err)) return fail(err);
                    if ((r.status >= 200 && r.status < 300) || r.status == 404) {
                        store.purge(id);
                        result.deleted++;
                        continue;
                    }
                    if (r.status != 412) return fail(httpError("Delete", r));
                }
                // changed on the server after we deleted it: the newer server version wins
                bool gone;
                if (!download(*n, n->href, remoteEtag, gone)) return false;
                if (gone) store.purge(id);
                continue;
            }

            if (!remoteChanged && n->dirty) {
                bool conflict;
                if (!upload(*n, conflict)) return false;
                if (!conflict) continue;
            } else if (!remoteChanged) {
                continue;
            }
            if (n->dirty) {
                keepLocalAsCopy(*n);
                n = store.find(id);
            }
            bool gone;
            if (!download(*n, n->href, remoteEtag, gone)) return false;
            if (gone) store.purge(id);
        }

        // 3. files that only exist on the server
        for (auto& kv : remote) {
            progress("Downloading " + std::to_string(++step) + "...");
            Note* n = store.create("");
            const std::string id = n->id;
            bool gone;
            const bool ok = download(*n, kv.second.href, kv.second.etag, gone);
            if (!ok || gone) store.purge(id);
            if (!ok) return false;
        }

        // 4. notes that only exist here
        ids.clear();
        for (auto& n : store.all()) ids.push_back(n.id);
        for (auto& id : ids) {
            Note* n = store.find(id);
            if (!n || !n->href.empty()) continue;
            if (n->deleted) {
                store.purge(id);
                continue;
            }
            if (!n->dirty) continue;
            progress("Uploading " + std::to_string(++step) + "...");
            bool conflict;
            if (!upload(*n, conflict)) return false;
        }
        return true;
    }
};

}  // namespace

SyncResult syncNotes(NotesStore& store, const SyncAccount& account, const DavClient::CertFn& onCert,
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

bool createFolder(const SyncAccount& account, const DavClient::CertFn& onCert, std::string& err) {
    DavClient client;
    client.onCert = onCert;
    if (!client.open(account.url, account.user, account.pass, err)) return false;
    dav::HttpResponse r;
    if (!client.request("MKCOL", client.url().path, {}, "", r, err)) return false;
    if (r.status == 201 || r.status == 405) return true;  // 405: it already exists
    if (r.status == 409) err = "The parent folder does not exist either. Create the folders on the server first.";
    else err = httpError("Creating the folder", r);
    return false;
}
