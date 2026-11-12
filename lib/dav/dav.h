// Portable building blocks for a WebDAV notes client: multistatus parsing,
// path encoding, HTTP response reading.
#pragma once
#include <cstddef>
#include <functional>
#include <string>

namespace dav {

// ---------------------------------------------------------------- WebDAV
struct Resource {
    std::string href;  // as sent by the server (usually an absolute, percent-encoded path)
    std::string etag;
    bool isCollection = false;
};

// Incremental parser for a 207 multistatus body: calls `fn` once per <response>.
class MultistatusParser {
public:
    using Fn = std::function<void(const Resource&)>;
    explicit MultistatusParser(Fn fn) : fn_(std::move(fn)) {}
    void feed(const char* data, size_t len);

private:
    Fn fn_;
    std::string buf_;
};

// PROPFIND body listing the members of a folder with their ETags.
const char* listFolderBody();

// A file name for a note ("Shopping list" -> "Shopping list"), without extension:
// characters that are awkward in file names are replaced, length is limited.
std::string fileStem(const std::string& title);
// Percent-encodes one path segment (UTF-8 safe) / decodes a path.
std::string encodeSegment(const std::string& segment);
std::string decodePath(const std::string& path);

// The human-readable message in a WebDAV error body (Sabre's <s:message>), or "".
std::string errorMessage(const std::string& body);

// ---------------------------------------------------------------- HTTP
struct Url {
    bool https = false;
    std::string host;
    int port = 80;
    std::string path;  // always starts with '/'
};
bool parseUrl(const std::string& text, Url& out);
// Turns an href from a multistatus (absolute URL or absolute path) into a path.
std::string hrefPath(const std::string& href);
std::string base64(const std::string& data);

struct HttpResponse {
    int status = 0;
    std::string etag;
    std::string location;    // Location header (redirects)
    std::string body;        // filled when no sink is given
    bool truncated = false;  // body was larger than maxBody
    bool keepAlive = false;  // connection can be reused
};

// read(buf, max) returns the number of bytes read, or <= 0 on EOF / timeout.
using ReadFn = std::function<int(char* buf, size_t max)>;
using SinkFn = std::function<void(const char* data, size_t len)>;

// Reads one HTTP/1.1 response. Returns false if the response was malformed or cut short.
bool readHttpResponse(const ReadFn& read, bool isHead, HttpResponse& out, const SinkFn& sink = nullptr,
                      size_t maxBody = 32768);

}  // namespace dav
