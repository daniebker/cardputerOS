#include "dav.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace dav {

namespace {

std::string lowerStr(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

// ---- tiny tolerant XML helpers (namespace prefixes are ignored) ----

// Finds <[prefix:]name ...> starting at `from`. Returns the index just after '>' or npos.
size_t findOpen(const std::string& s, const char* name, size_t from, bool* selfClosed = nullptr) {
    const size_t nlen = strlen(name);
    size_t i = from;
    while ((i = s.find('<', i)) != std::string::npos) {
        size_t j = i + 1;
        if (j < s.size() && (s[j] == '/' || s[j] == '?' || s[j] == '!')) {
            i = j;
            continue;
        }
        size_t end = j;
        while (end < s.size() && s[end] != '>' && s[end] != ' ' && s[end] != '/' && s[end] != '\t' && s[end] != '\n' &&
               s[end] != '\r')
            end++;
        size_t colon = s.rfind(':', end);
        size_t local = (colon != std::string::npos && colon >= j && colon < end) ? colon + 1 : j;
        if (end - local == nlen && lowerStr(s.substr(local, nlen)) == name) {
            size_t gt = s.find('>', end);
            if (gt == std::string::npos) return std::string::npos;
            if (selfClosed) *selfClosed = gt > 0 && s[gt - 1] == '/';
            return gt + 1;
        }
        i = end;
    }
    return std::string::npos;
}

// Finds </[prefix:]name> at or after `from`; returns the index of '<' or npos. `after` gets the index past '>'.
size_t findClose(const std::string& s, const char* name, size_t from, size_t* after = nullptr) {
    const size_t nlen = strlen(name);
    size_t i = from;
    while ((i = s.find("</", i)) != std::string::npos) {
        size_t j = i + 2;
        size_t gt = s.find('>', j);
        if (gt == std::string::npos) return std::string::npos;
        std::string tag = trim(s.substr(j, gt - j));
        size_t colon = tag.rfind(':');
        if (colon != std::string::npos) tag = tag.substr(colon + 1);
        if (tag.size() == nlen && lowerStr(tag) == name) {
            if (after) *after = gt + 1;
            return i;
        }
        i = gt;
    }
    return std::string::npos;
}

std::string xmlUnescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') {
            out.push_back(s[i]);
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 8) {
            out.push_back('&');
            continue;
        }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out.push_back('&');
        else if (ent == "lt") out.push_back('<');
        else if (ent == "gt") out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos") out.push_back('\'');
        else if (ent.size() > 1 && ent[0] == '#') {
            long cp = (ent[1] == 'x' || ent[1] == 'X') ? strtol(ent.c_str() + 2, nullptr, 16) : strtol(ent.c_str() + 1, nullptr, 10);
            if (cp > 0 && cp < 0x80) out.push_back((char)cp);
        } else {
            out.append(s, i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

std::string elementText(const std::string& s, const char* name) {
    bool selfClosed = false;
    size_t start = findOpen(s, name, 0, &selfClosed);
    if (start == std::string::npos || selfClosed) return std::string();
    size_t end = findClose(s, name, start);
    if (end == std::string::npos) return std::string();
    std::string v = s.substr(start, end - start);
    if (v.compare(0, 9, "<![CDATA[") == 0) {
        size_t e = v.find("]]>");
        return v.substr(9, e == std::string::npos ? std::string::npos : e - 9);
    }
    return xmlUnescape(trim(v));
}

}  // namespace

// ---------------------------------------------------------------- WebDAV

const char* listFolderBody() {
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
           "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:getetag/><d:resourcetype/></d:prop></d:propfind>";
}

void MultistatusParser::feed(const char* data, size_t len) {
    buf_.append(data, len);
    for (;;) {
        size_t after = 0;
        size_t close = findClose(buf_, "response", 0, &after);
        if (close == std::string::npos) break;
        std::string block = buf_.substr(0, close);
        buf_.erase(0, after);
        size_t open = findOpen(block, "response", 0);
        if (open == std::string::npos) continue;
        block.erase(0, open);
        Resource r;
        r.href = elementText(block, "href");
        r.etag = elementText(block, "getetag");
        bool selfClosed = false;
        size_t rt = findOpen(block, "resourcetype", 0, &selfClosed);
        if (rt != std::string::npos && !selfClosed) {
            size_t end = findClose(block, "resourcetype", rt);
            if (end != std::string::npos) r.isCollection = findOpen(block.substr(rt, end - rt), "collection", 0) != std::string::npos;
        }
        if (!r.href.empty() && fn_) fn_(r);
    }
    // never let a hostile / broken body grow without bound
    if (buf_.size() > 16384) buf_.erase(0, buf_.size() - 4096);
}

std::string fileStem(const std::string& title) {
    std::string t;
    for (char c : title) {
        unsigned char u = (unsigned char)c;
        if (u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|' || c == '#' || c == '%')
            t.push_back(' ');
        else
            t.push_back(c);
    }
    // collapse spaces, trim, drop leading dots (hidden files)
    std::string out;
    for (char c : t) {
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out.push_back(c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out[0] == '.') out.erase(0, 1);
    if (out.size() > 60) {
        size_t cut = 60;
        while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) cut--;  // keep UTF-8 intact
        out.resize(cut);
        while (!out.empty() && out.back() == ' ') out.pop_back();
    }
    return out.empty() ? std::string("Untitled") : out;
}

std::string encodeSegment(const std::string& segment) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : segment) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string decodePath(const std::string& path) {
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < path.size(); i++) {
        if (path[i] == '%' && i + 2 < path.size() && val(path[i + 1]) >= 0 && val(path[i + 2]) >= 0) {
            out.push_back((char)(val(path[i + 1]) * 16 + val(path[i + 2])));
            i += 2;
        } else {
            out.push_back(path[i]);
        }
    }
    return out;
}

std::string errorMessage(const std::string& body) {
    std::string m = elementText(body, "message");
    if (m.size() > 160) m = m.substr(0, 160) + "...";
    return m;
}

// ---------------------------------------------------------------- HTTP

bool parseUrl(const std::string& textIn, Url& out) {
    std::string text = trim(textIn);
    std::string lower = lowerStr(text.substr(0, 8));
    size_t start;
    if (lower.compare(0, 8, "https://") == 0) {
        out.https = true;
        out.port = 443;
        start = 8;
    } else if (lower.compare(0, 7, "http://") == 0) {
        out.https = false;
        out.port = 80;
        start = 7;
    } else {
        return false;
    }
    size_t slash = text.find('/', start);
    std::string hostPort = text.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    out.path = slash == std::string::npos ? "/" : text.substr(slash);
    if (hostPort.find('@') != std::string::npos) return false;  // credentials belong in their own fields
    size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos && hostPort.find(']') == std::string::npos) {
        int p = atoi(hostPort.c_str() + colon + 1);
        if (p <= 0 || p > 65535) return false;
        out.port = p;
        hostPort.erase(colon);
    }
    out.host = hostPort;
    for (char c : out.path)
        if ((unsigned char)c <= ' ') return false;
    return !out.host.empty();
}

std::string hrefPath(const std::string& href) {
    std::string h = trim(href);
    std::string lower = lowerStr(h.substr(0, 8));
    if (lower.compare(0, 7, "http://") == 0 || lower.compare(0, 8, "https://") == 0) {
        size_t slash = h.find('/', h.find("//") + 2);
        return slash == std::string::npos ? "/" : h.substr(slash);
    }
    return h;
}

std::string base64(const std::string& data) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t v = ((unsigned char)data[i] << 16) | ((unsigned char)data[i + 1] << 8) | (unsigned char)data[i + 2];
        out.push_back(tbl[v >> 18]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back(tbl[v & 63]);
        i += 3;
    }
    if (i + 1 == data.size()) {
        uint32_t v = (unsigned char)data[i] << 16;
        out.push_back(tbl[v >> 18]);
        out.push_back(tbl[(v >> 12) & 63]);
        out += "==";
    } else if (i + 2 == data.size()) {
        uint32_t v = ((unsigned char)data[i] << 16) | ((unsigned char)data[i + 1] << 8);
        out.push_back(tbl[v >> 18]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

namespace {
struct Reader {
    const ReadFn& read;
    char buf[512];
    int pos = 0, len = 0;
    bool eof = false;

    explicit Reader(const ReadFn& r) : read(r) {}
    bool fill() {
        if (eof) return false;
        len = read(buf, sizeof buf);
        pos = 0;
        if (len <= 0) {
            len = 0;
            eof = true;
            return false;
        }
        return true;
    }
    int get() {
        if (pos >= len && !fill()) return -1;
        return (unsigned char)buf[pos++];
    }
    bool line(std::string& out, size_t maxLen = 4096) {
        out.clear();
        for (;;) {
            int c = get();
            if (c < 0) return false;
            if (c == '\n') break;
            if (out.size() < maxLen) out.push_back((char)c);
        }
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
    }
    // Passes up to n body bytes to `fn`; returns false if the stream ended early.
    template <typename F>
    bool body(size_t n, F&& fn) {
        while (n > 0) {
            if (pos >= len && !fill()) return false;
            size_t take = (size_t)(len - pos);
            if (take > n) take = n;
            fn(buf + pos, take);
            pos += (int)take;
            n -= take;
        }
        return true;
    }
};
}  // namespace

bool readHttpResponse(const ReadFn& read, bool isHead, HttpResponse& out, const SinkFn& sink, size_t maxBody) {
    out = HttpResponse();
    Reader rd(read);
    std::string line;

    // status line (skipping 1xx interim responses)
    for (;;) {
        if (!rd.line(line)) return false;
        if (line.compare(0, 5, "HTTP/") != 0) return false;
        size_t sp = line.find(' ');
        if (sp == std::string::npos) return false;
        out.status = atoi(line.c_str() + sp + 1);
        const bool http11 = line.compare(0, 8, "HTTP/1.1") == 0;
        out.keepAlive = http11;

        long contentLength = -1;
        bool chunked = false;
        out.etag.clear();
        out.location.clear();
        for (;;) {
            if (!rd.line(line)) return false;
            if (line.empty()) break;
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string name = lowerStr(line.substr(0, colon));
            std::string value = trim(line.substr(colon + 1));
            if (name == "content-length") contentLength = atol(value.c_str());
            else if (name == "transfer-encoding") chunked = lowerStr(value).find("chunked") != std::string::npos;
            else if (name == "etag") out.etag = value;
            else if (name == "location") out.location = value;
            else if (name == "connection") {
                std::string v = lowerStr(value);
                if (v.find("close") != std::string::npos) out.keepAlive = false;
                else if (v.find("keep-alive") != std::string::npos) out.keepAlive = true;
            }
        }
        if (out.status >= 100 && out.status < 200) continue;

        auto deliver = [&](const char* d, size_t n) {
            if (sink) {
                sink(d, n);
                return;
            }
            size_t room = out.body.size() < maxBody ? maxBody - out.body.size() : 0;
            if (n > room) {
                out.truncated = true;
                n = room;
            }
            out.body.append(d, n);
        };

        if (isHead || out.status == 204 || out.status == 304) return true;
        if (chunked) {
            for (;;) {
                if (!rd.line(line)) return false;
                if (line.empty()) continue;
                long size = strtol(line.c_str(), nullptr, 16);
                if (size < 0) return false;
                if (size == 0) {
                    // trailers until an empty line
                    while (rd.line(line) && !line.empty()) {
                    }
                    return true;
                }
                if (!rd.body((size_t)size, deliver)) return false;
            }
        }
        if (contentLength >= 0) return rd.body((size_t)contentLength, deliver);
        // no length: the body runs until the connection closes
        out.keepAlive = false;
        for (;;) {
            if (rd.pos >= rd.len && !rd.fill()) return true;
            deliver(rd.buf + rd.pos, (size_t)(rd.len - rd.pos));
            rd.pos = rd.len;
        }
    }
}

}  // namespace dav
