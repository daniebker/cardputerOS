#include "tasks_sync.h"

#include "../../hal/sdcard.h"
#include "../../os/secrets.h"
#include "../../os/storage.h"

namespace {
const char* kDownload = "/cardputerOS/tasks/download.tmp";
const char* kUpload = "/cardputerOS/tasks/upload.tmp";
fs::FS& sd() { return hal::sdFs(); }

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

bool endsWithJson(const std::string& s) {
    if (s.size() < 6) return false;
    std::string tail = s.substr(s.size() - 5);
    for (char& c : tail) c = (char)tolower((unsigned char)c);
    return tail == ".json";
}
}  // namespace

TasksAccount TasksAccount::load() {
    TasksAccount a;
    a.url = storage::getStr("mw_url");
    a.user = storage::getStr("mw_user");
    a.pass = secrets::get("mw_pass");
    return a;
}

void TasksAccount::save() const {
    storage::putStr("mw_url", url);
    storage::putStr("mw_user", user);
    secrets::put("mw_pass", pass);
}

void TasksAccount::split(std::string& folder, std::string& file) const {
    std::string u = url;
    while (u.size() > 8 && u.back() == '/') u.pop_back();
    const size_t slash = u.rfind('/');
    if (endsWithJson(u) && slash != std::string::npos && slash > 8) {
        folder = u.substr(0, slash + 1);
        file = dav::decodePath(u.substr(slash + 1));
    } else {
        folder = u + "/";
        file = "data.json";
    }
}

namespace {

struct Sync {
    TasksStore& store;
    DavClient& client;
    std::string fileName, deviceId;
    TasksSyncResult result;
    const std::function<void(const std::string&)>& progress;

    Sync(TasksStore& s, DavClient& c, const std::function<void(const std::string&)>& p) : store(s), client(c), progress(p) {}

    bool fail(const std::string& msg) {
        if (result.error.empty()) result.error = msg;
        return false;
    }

    std::string href() const { return client.url().path + dav::encodeSegment(fileName); }

    // GET the file into the local copy (unless it did not change).
    bool download(bool conditional) {
        progress("Downloading tasks...");
        std::vector<DavClient::Header> h;
        if (conditional && store.hasData() && !store.etag().empty()) h.push_back({"If-None-Match", store.etag()});
        File f = sd().open(kDownload, "w");
        if (!f) return fail("Could not write to the SD card (full?).");
        bool writeOk = true;
        size_t total = 0;
        std::string head;  // start of the body, for error messages
        dav::HttpResponse r;
        std::string err;
        const bool ok = client.request("GET", href(), h, std::string(), r, err, [&](const char* d, size_t n) {
            if (head.size() < 1024) head.append(d, std::min(n, 1024 - head.size()));
            if (writeOk && f.write((const uint8_t*)d, n) != n) writeOk = false;
            total += n;
        });
        f.close();
        r.body = head;
        if (!ok) {
            sd().remove(kDownload);
            return fail(err);
        }
        if (r.status == 304) {
            sd().remove(kDownload);
            return true;
        }
        if (r.status != 200) {
            sd().remove(kDownload);
            if (r.status == 404)
                return fail("There is no " + fileName + " in this folder yet. Set up WebDAV sync in Mindwtr with "
                            "the same folder and sync once, then try again.");
            return fail(httpError("Download", r));
        }
        if (!writeOk) {
            sd().remove(kDownload);
            return fail("Could not save the task file on the SD card (" + std::to_string(total / 1024) + " KB; card full?).");
        }
        std::string e;
        if (!store.adoptDownload(kDownload, r.etag, e)) return fail(e);
        result.downloaded = true;
        return true;
    }

    bool upload() {
        for (int attempt = 0; attempt < 3; attempt++) {
            const int count = store.pendingCount();
            progress("Uploading " + std::to_string(count) + (count == 1 ? " change..." : " changes..."));
            size_t size = 0;
            std::string e;
            if (!store.buildUpload(kUpload, deviceId, size, e)) return fail(e);
            File f = sd().open(kUpload, "r");
            if (!f) return fail("Could not read the task file on the SD card.");
            DavClient::BodySource body;
            body.size = size;
            body.rewind = [&f]() { return f.seek(0); };
            body.read = [&f](uint8_t* buf, size_t max) { return (int)f.read(buf, max); };
            std::vector<DavClient::Header> h = {{"Content-Type", "application/json"}};
            if (!store.etag().empty()) h.push_back({"If-Match", store.etag()});
            dav::HttpResponse r;
            std::string err;
            const bool ok = client.request("PUT", href(), h, body, r, err);
            f.close();
            if (!ok) {
                sd().remove(kUpload);
                return fail(err);
            }
            if (r.status == 412) {
                // changed on the server since we downloaded it: get it and replay the edits
                sd().remove(kUpload);
                if (!download(false)) return false;
                continue;
            }
            if (r.status < 200 || r.status >= 300) {
                sd().remove(kUpload);
                return fail(httpError("Upload", r));
            }
            if (!store.uploaded(kUpload, r.etag, e)) return fail(e);
            result.uploaded += count;
            return true;
        }
        return fail("The task file on the server keeps changing; try again in a moment.");
    }
};

}  // namespace

TasksSyncResult syncTasks(TasksStore& store, const TasksAccount& account, const std::string& deviceId,
                          const DavClient::CertFn& onCert, const std::function<void(const std::string&)>& progress) {
    DavClient client;
    client.onCert = onCert;
    Sync s(store, client, progress);
    hal::SdSession session;  // the card stays mounted for the whole sync
    if (!session.ok() || !hal::sdMakeDirs(TasksStore::kDir)) {
        s.result.error = "No microSD card. Tasks keeps its data on the card: insert one and try again.";
        return s.result;
    }
    std::string folder;
    account.split(folder, s.fileName);
    std::string err;
    if (!client.open(folder, account.user, account.pass, err)) {
        s.result.error = err;
        return s.result;
    }
    s.deviceId = deviceId;
    s.result.ok = s.download(true) && (store.pendingCount() == 0 || s.upload());
    client.close();
    return s.result;
}
