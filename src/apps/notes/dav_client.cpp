#include "dav_client.h"

#include <Arduino.h>

#include "../../os/storage.h"

namespace {
constexpr uint32_t kReadTimeoutMs = 15000;
constexpr int kConnectTimeoutMs = 10000;

// NVS: pinned certificates, one "host:port<TAB>sha256hex" line each
std::string pinnedCert(const std::string& id) {
    for (auto& line : storage::splitLines(storage::getStr("dav_certs"))) {
        auto f = storage::splitFields(line);
        if (f.size() == 2 && f[0] == id) return f[1];
    }
    return std::string();
}

void pinCert(const std::string& id, const std::string& fp) {
    std::string text;
    auto lines = storage::splitLines(storage::getStr("dav_certs"));
    size_t skip = lines.size() >= 8 ? lines.size() - 7 : 0;
    for (size_t i = skip; i < lines.size(); i++) {
        auto f = storage::splitFields(lines[i]);
        if (f.size() == 2 && f[0] != id) text += lines[i] + "\n";
    }
    text += id + "\t" + fp + "\n";
    storage::putStr("dav_certs", text);
}
}  // namespace

bool DavClient::open(const std::string& url, const std::string& user, const std::string& pass, std::string& err) {
    close();
    if (!dav::parseUrl(url, url_)) {
        err = "The server address must start with https:// or http://";
        return false;
    }
    if (url_.path.empty() || url_.path.back() != '/') url_.path += '/';
    auth_ = user.empty() ? std::string() : "Basic " + dav::base64(user + ":" + pass);
    acceptedCert_.clear();
    return true;
}

void DavClient::close() {
    if (conn_) conn_->stop();
    conn_ = nullptr;
}

bool DavClient::connected() { return conn_ && conn_->connected(); }

bool DavClient::connect(std::string& err) {
    close();
    if (!url_.https) {
        if (!plain_.connect(url_.host.c_str(), url_.port, kConnectTimeoutMs)) {
            err = "Could not connect to " + url_.host;
            return false;
        }
        conn_ = &plain_;
        return true;
    }

    // The certificate chain is not validated against a CA list; instead the
    // server certificate is pinned the first time it is seen (like SSH host keys).
    tls_.setInsecure();
    tls_.setHandshakeTimeout(15);
    if (!tls_.connect(url_.host.c_str(), url_.port, kConnectTimeoutMs)) {
        err = "Could not connect to " + url_.host + " (TLS)";
        return false;
    }
    uint8_t hash[32];
    if (!tls_.getFingerprintSHA256(hash)) {
        tls_.stop();
        err = "Could not read the server certificate";
        return false;
    }
    std::string fp;
    static const char hex[] = "0123456789abcdef";
    for (uint8_t b : hash) {
        fp.push_back(hex[b >> 4]);
        fp.push_back(hex[b & 15]);
    }
    if (fp != acceptedCert_) {
        const std::string id = url_.host + ":" + std::to_string(url_.port);
        const std::string known = pinnedCert(id);
        if (known != fp) {
            // Nothing (in particular no password) has been sent yet.
            if (!onCert || !onCert(url_.host, fp, !known.empty())) {
                tls_.stop();
                err = "Server certificate not accepted";
                return false;
            }
            pinCert(id, fp);
        }
        acceptedCert_ = fp;
    }
    conn_ = &tls_;
    return true;
}

bool DavClient::request(const char* method, const std::string& path, const std::vector<Header>& headers,
                        const std::string& body, dav::HttpResponse& resp, std::string& err, const dav::SinkFn& sink) {
    return send(method, path, headers, body.size(),
                [&body](Client* c) {
                    return body.empty() || c->write((const uint8_t*)body.data(), body.size()) == body.size();
                },
                resp, err, sink);
}

bool DavClient::request(const char* method, const std::string& path, const std::vector<Header>& headers,
                        const BodySource& body, dav::HttpResponse& resp, std::string& err, const dav::SinkFn& sink) {
    return send(method, path, headers, body.size,
                [&body](Client* c) {
                    if (body.rewind && !body.rewind()) return false;
                    uint8_t buf[1024];
                    size_t left = body.size;
                    while (left > 0) {
                        const int n = body.read(buf, std::min(left, sizeof buf));
                        if (n <= 0) return false;
                        size_t off = 0;
                        while (off < (size_t)n) {
                            const size_t w = c->write(buf + off, (size_t)n - off);
                            if (w == 0) return false;
                            off += w;
                        }
                        left -= (size_t)n;
                    }
                    return true;
                },
                resp, err, sink);
}

bool DavClient::send(const char* method, const std::string& path, const std::vector<Header>& headers, size_t bodyLen,
                     const std::function<bool(Client*)>& writeBody, dav::HttpResponse& resp, std::string& err,
                     const dav::SinkFn& sink) {
    std::string head = std::string(method) + " " + path + " HTTP/1.1\r\nHost: " + url_.host;
    if (url_.port != (url_.https ? 443 : 80)) head += ":" + std::to_string(url_.port);
    head += "\r\nUser-Agent: cardputerOS\r\nAccept: */*\r\nConnection: keep-alive\r\n";
    if (!auth_.empty()) head += "Authorization: " + auth_ + "\r\n";
    for (auto& h : headers) head += std::string(h.name) + ": " + h.value + "\r\n";
    const bool hasBody = bodyLen > 0 || !strcmp(method, "PUT") || !strcmp(method, "REPORT") || !strcmp(method, "PROPFIND");
    if (hasBody) head += "Content-Length: " + std::to_string(bodyLen) + "\r\n";
    head += "\r\n";

    // A kept-alive connection may have been closed by the server in the meantime: retry once.
    for (int attempt = 0; attempt < 2; attempt++) {
        const bool reused = connected();
        if (!reused && !connect(err)) return false;

        bool sent = conn_->write((const uint8_t*)head.data(), head.size()) == head.size();
        if (sent) sent = writeBody(conn_);

        size_t received = 0;
        bool ok = false;
        if (sent) {
            Client* c = conn_;
            dav::ReadFn read = [c, &received](char* buf, size_t max) -> int {
                const uint32_t t0 = millis();
                for (;;) {
                    int avail = c->available();
                    if (avail > 0) {
                        int n = c->read((uint8_t*)buf, std::min<size_t>(max, (size_t)avail));
                        if (n > 0) received += n;
                        return n;
                    }
                    if (!c->connected() || millis() - t0 > kReadTimeoutMs) return 0;
                    delay(5);
                }
            };
            ok = dav::readHttpResponse(read, !strcmp(method, "HEAD"), resp, sink);
        }
        if (ok) {
            if (!resp.keepAlive) close();
            return true;
        }
        close();
        if (!(reused && received == 0)) break;  // only a stale connection is worth a retry
    }
    err = "No valid answer from " + url_.host;
    return false;
}
