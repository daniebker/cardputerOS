#include "calendar_fetch.h"

#include <Arduino.h>

#include <cstring>

namespace {
constexpr int kMaxRedirects = 5;
constexpr size_t kBufferSize = 4096;
constexpr uint32_t kTaskStack = 20480;  // the TLS handshake is stack hungry

std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string httpError(const dav::HttpResponse& r) {
    switch (r.status) {
        case 401: return "The server wants a user name and password (or rejected them).";
        case 403: return "The server refused the link (no permission). It may have been reset or made private.";
        case 404:
        case 410: return "The calendar was not found at this link. It may have been reset or deleted.";
        default: break;
    }
    if (r.status >= 500) return "The server had a problem (HTTP " + std::to_string(r.status) + "). Try again later.";
    return "Download failed (HTTP " + std::to_string(r.status) + ").";
}
}  // namespace

std::string calendarHttpUrl(const std::string& link) {
    size_t a = 0, b = link.size();
    while (a < b && isspace((unsigned char)link[a])) a++;
    while (b > a && isspace((unsigned char)link[b - 1])) b--;
    std::string url = link.substr(a, b - a);
    const std::string l = lower(url.substr(0, 10));
    if (l.compare(0, 10, "webcals://") == 0) url = "https://" + url.substr(10);
    else if (l.compare(0, 9, "webcal://") == 0) url = "https://" + url.substr(9);
    return url;
}

FetchResult fetchCalendar(const FetchRequest& req, const DavClient::CertFn& onCert,
                          const std::function<bool(const char*, size_t)>& sink) {
    FetchResult res;
    std::string url = calendarHttpUrl(req.url);
    dav::Url first;
    if (!dav::parseUrl(url, first)) {
        res.error = "The link must start with https://, http:// or webcal://";
        return res;
    }
    for (int hop = 0; hop <= kMaxRedirects; hop++) {
        dav::Url u;
        if (!dav::parseUrl(url, u)) {
            res.error = "The server redirected to an address that cannot be used.";
            return res;
        }
        // credentials only go to the server they were given for
        const bool sameServer = u.host == first.host && u.port == first.port && u.https == first.https;
        DavClient client;
        client.onCert = onCert;
        std::string err;
        if (!client.open(dav::origin(u) + "/", sameServer ? req.user : std::string(), req.pass, err)) {
            res.error = err;
            return res;
        }
        std::vector<DavClient::Header> h;
        if (!req.etag.empty()) h.push_back({"If-None-Match", req.etag});
        if (!req.lastModified.empty()) h.push_back({"If-Modified-Since", req.lastModified});
        dav::HttpResponse r;
        bool sinkOk = true;
        size_t bytes = 0;
        const bool ok = client.request("GET", u.path, h, std::string(), r, err, [&](const char* d, size_t n) {
            if (r.status != 200 || !sinkOk) return;  // error pages and redirect bodies are not kept
            bytes += n;
            if (!sink(d, n)) sinkOk = false;
        });
        client.close();
        if (!ok) {
            res.error = err;
            return res;
        }
        res.status = r.status;
        if (r.status >= 300 && r.status < 400 && r.status != 304 && !r.location.empty()) {
            url = dav::resolveUrl(u, r.location);
            continue;
        }
        if (r.status == 304) {
            res.ok = true;
            res.etag = r.etag.empty() ? req.etag : r.etag;
            res.lastModified = r.lastModified.empty() ? req.lastModified : r.lastModified;
            return res;
        }
        if (r.status != 200) {
            res.error = httpError(r);
            return res;
        }
        if (!sinkOk) {
            res.error = "Could not save the calendar on the SD card (card full or removed?).";
            return res;
        }
        res.ok = true;
        res.etag = r.etag;
        res.lastModified = r.lastModified;
        res.bytes = bytes;
        return res;
    }
    res.error = "Too many redirects.";
    return res;
}

// ------------------------------------------------------------ FetchJob

FetchJob::~FetchJob() {
    if (buffer_) vStreamBufferDelete(buffer_);
}

std::shared_ptr<FetchJob> FetchJob::start(const FetchRequest& req) {
    std::shared_ptr<FetchJob> job(new FetchJob());
    job->req_ = req;
    job->buffer_ = xStreamBufferCreate(kBufferSize, 1);
    if (!job->buffer_) return nullptr;
    // the task keeps its own reference, so the app may drop the job at any time
    auto* ref = new std::shared_ptr<FetchJob>(job);
    if (xTaskCreate(taskEntry, "calfetch", kTaskStack, ref, 1, nullptr) != pdPASS) {
        delete ref;
        return nullptr;
    }
    return job;
}

void FetchJob::taskEntry(void* arg) {
    auto* ref = static_cast<std::shared_ptr<FetchJob>*>(arg);
    FetchJob& job = **ref;
    // In the background no one can be asked about a certificate: new or changed
    // certificates are refused (a refresh with r asks).
    job.result_ = fetchCalendar(job.req_, nullptr, [&job](const char* d, size_t n) {
        while (n > 0) {
            if (job.cancelled_) return false;
            const size_t sent = xStreamBufferSend(job.buffer_, d, n, pdMS_TO_TICKS(200));
            d += sent;
            n -= sent;
        }
        return true;
    });
    if (job.cancelled_ && job.result_.ok && job.result_.status == 200) {
        job.result_.ok = false;
        job.result_.error = "Cancelled.";
    }
    job.done_ = true;
    delete ref;  // may free the job if the app let go of it
    vTaskDelete(nullptr);
}

bool FetchJob::pump(const std::function<bool(const char*, size_t)>& sink) {
    // read `done` before draining, so nothing sent before it was set is missed
    const bool finished = done_;
    char buf[1024];
    for (int i = 0; i < 16; i++) {
        const size_t n = xStreamBufferReceive(buffer_, buf, sizeof buf, 0);
        if (n == 0) break;
        if (!cancelled_ && !sink(buf, n)) cancelled_ = true;
    }
    return finished && xStreamBufferIsEmpty(buffer_) == pdTRUE;
}
