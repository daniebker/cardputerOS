// Minimal HTTP/1.1 client for WebDAV: Basic auth, keep-alive, TLS with
// trust-on-first-use certificate pinning.
#pragma once
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <functional>
#include <string>
#include <vector>

#include "dav.h"

class DavClient {
public:
    struct Header {
        const char* name;
        std::string value;
    };
    // Asked when a TLS server is seen for the first time (changed = false) or
    // presents a different certificate than last time (changed = true).
    using CertFn = std::function<bool(const std::string& host, const std::string& fingerprint, bool changed)>;

    ~DavClient() { close(); }

    bool open(const std::string& url, const std::string& user, const std::string& pass, std::string& err);
    void close();
    const dav::Url& url() const { return url_; }

    // Returns false on a transport-level failure (err explains). HTTP errors are in resp.status.
    bool request(const char* method, const std::string& path, const std::vector<Header>& headers,
                 const std::string& body, dav::HttpResponse& resp, std::string& err,
                 const dav::SinkFn& sink = nullptr);

    CertFn onCert;

private:
    bool connect(std::string& err);
    bool connected();

    dav::Url url_;
    std::string auth_;
    WiFiClient plain_;
    WiFiClientSecure tls_;
    Client* conn_ = nullptr;
    std::string acceptedCert_;
};
