// app.ini parsing and path rules for Lua apps.
#include <cctype>
#include <cstring>

#include "script.h"

namespace script {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

struct NamedColor {
    const char* name;
    uint32_t rgb;
};
// The Dracula palette used by the rest of the UI, plus a few plain names.
const NamedColor kColors[] = {
    {"foreground", 0xF8F8F2}, {"white", 0xF8F8F2}, {"comment", 0x6272A4}, {"grey", 0x6272A4},
    {"gray", 0x6272A4},       {"cyan", 0x8BE9FD},  {"green", 0x50FA7B},   {"orange", 0xFFB86C},
    {"pink", 0xFF79C6},       {"purple", 0xBD93F9}, {"red", 0xFF5555},    {"yellow", 0xF1FA8C},
    {"background", 0x282A36}, {"black", 0x000000},
};

}  // namespace

bool validId(const std::string& id) {
    if (id.empty() || id.size() > 31 || id[0] == '.') return false;
    for (char c : id)
        if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.') return false;
    return true;
}

bool parseColor(const std::string& text, uint32_t& out) {
    const std::string t = lower(trim(text));
    if (t.size() == 7 && t[0] == '#') {
        uint32_t v = 0;
        for (size_t i = 1; i < 7; i++) {
            const char c = t[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else return false;
            v = v * 16 + (uint32_t)d;
        }
        out = v;
        return true;
    }
    for (const auto& nc : kColors) {
        if (t == nc.name) {
            out = nc.rgb;
            return true;
        }
    }
    return false;
}

bool parseManifest(const std::string& id, const std::string& text, Manifest& out, std::string& error) {
    out = Manifest();
    out.id = id;
    out.name = id;
    out.description = "SD card app";
    size_t pos = 0;
    int lineNo = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        lineNo++;
        if (lineNo == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line = trim(line.substr(3));  // UTF-8 BOM
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            error = "app.ini line " + std::to_string(lineNo) + ": expected key = value";
            return false;
        }
        const std::string k = lower(trim(line.substr(0, eq)));
        std::string v = trim(line.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        if (v.size() > 120) v.resize(120);
        if (k == "name") {
            if (!v.empty()) out.name = v.substr(0, 40);
        } else if (k == "description") {
            out.description = v;
        } else if (k == "color" || k == "colour") {
            if (!parseColor(v, out.color)) {
                error = "app.ini line " + std::to_string(lineNo) + ": unknown colour \"" + v + "\"";
                return false;
            }
        } else if (k == "icon") {
            out.icon = lower(v);
        } else if (k == "folder") {
            out.folder = v.substr(0, 24);
        }
    }
    return true;
}

bool cleanPath(const std::string& in, std::string& out) {
    out.clear();
    if (in.size() > 128) return false;
    size_t pos = 0;
    while (pos <= in.size()) {
        size_t sl = in.find('/', pos);
        if (sl == std::string::npos) sl = in.size();
        const std::string part = in.substr(pos, sl - pos);
        pos = sl + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") return false;
        for (char c : part)
            if ((unsigned char)c < 0x20 || c == '\\' || c == ':' || c == 0x7F) return false;
        if (!out.empty()) out += '/';
        out += part;
    }
    return true;
}

}  // namespace script
