// Downloads a calendar (.ics) link: conditional GET (If-None-Match /
// If-Modified-Since), redirects, webcal:// links, optional Basic auth.
//
// fetchCalendar() blocks; FetchJob runs it on a FreeRTOS task so the app stays
// usable while a calendar is revalidated. The SD card shares the SPI bus with
// the display, so the worker never touches the card: it hands the body to the
// main loop through a small stream buffer and the app writes it to the card in
// pump().
#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "../notes/dav_client.h"

struct FetchRequest {
    std::string url, user, pass;
    std::string etag, lastModified;  // validators of the cached copy ("" = download in full)
};

struct FetchResult {
    bool ok = false;     // a usable answer: 200 (new copy) or 304 (cached copy still current)
    int status = 0;
    std::string etag, lastModified;
    std::string error;   // when !ok
    size_t bytes = 0;    // body bytes of a 200
};

// "webcal://x" -> "https://x"; trims spaces.
std::string calendarHttpUrl(const std::string& link);

// Body bytes of a 200 answer go to `sink` (return false to give up: the transfer
// is then reported as failed). `onCert` as for DavClient (nullptr: reject new
// and changed certificates).
FetchResult fetchCalendar(const FetchRequest& req, const DavClient::CertFn& onCert,
                          const std::function<bool(const char*, size_t)>& sink);

class FetchJob {
public:
    ~FetchJob();
    // Starts the download on its own task. nullptr if there is not enough memory.
    static std::shared_ptr<FetchJob> start(const FetchRequest& req);

    // Main loop: passes the bytes received so far to `sink`. Returns true once the
    // download has finished and everything was passed on (result() is then valid).
    // If `sink` returns false the job is cancelled.
    bool pump(const std::function<bool(const char*, size_t)>& sink);
    void cancel() { cancelled_ = true; }
    const FetchResult& result() const { return result_; }

private:
    FetchJob() {}
    static void taskEntry(void* arg);

    FetchRequest req_;
    FetchResult result_;
    StreamBufferHandle_t buffer_ = nullptr;
    std::atomic<bool> done_{false}, cancelled_{false};
};
