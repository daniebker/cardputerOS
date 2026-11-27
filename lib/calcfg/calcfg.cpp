#include "calcfg.h"

#include <algorithm>
#include <cctype>

namespace calcfg {

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    if (s.compare(0, 3, "\xEF\xBB\xBF") == 0) a = 3;  // UTF-8 BOM (Notepad)
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Position of the link in the line, or npos.
size_t linkStart(const std::string& line) {
    const std::string l = lower(line);
    size_t best = std::string::npos;
    for (const char* scheme : {"https://", "http://", "webcal://", "webcals://"}) {
        const size_t p = l.find(scheme);
        if (p != std::string::npos && (p < best || best == std::string::npos)) best = p;
    }
    return best;
}
}  // namespace

File parse(const std::string& text) {
    File out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t at = linkStart(line);
        if (at == std::string::npos) {
            out.unreadable.push_back(line);
            continue;
        }
        size_t end = at;
        while (end < line.size() && !isspace((unsigned char)line[end])) end++;
        Line c;
        c.url = line.substr(at, end - at);
        // the name: what comes before the link, without a separator at its end
        std::string name = trim(line.substr(0, at));
        while (!name.empty() && (name.back() == ':' || name.back() == '=' || name.back() == '|' || name.back() == ',' ||
                                 name.back() == ';' || isspace((unsigned char)name.back())))
            name.pop_back();
        if (name.size() >= 2 && (name[0] == '"' || name[0] == '\'') && name.back() == name[0])
            name = name.substr(1, name.size() - 2);
        c.name = trim(name);
        out.calendars.push_back(c);
    }
    return out;
}

bool isImportName(const std::string& fileName) {
    std::string n = lower(fileName);
    const size_t slash = n.rfind('/');
    if (slash != std::string::npos) n = n.substr(slash + 1);
    if (n.empty() || n[0] == '.') return false;  // hidden, or macOS "._name"
    if (n.compare(0, 8, "calendar") != 0) return false;
    return n.size() >= 4 && n.compare(n.size() - 4, 4, ".txt") == 0;
}

std::string decode(const std::string& raw, std::string& problem) {
    problem.clear();
    if (raw.compare(0, 5, "{\\rtf") == 0) {
        problem = "The file is rich text (RTF). Save it as plain text (in TextEdit: Format > Make Plain Text).";
        return std::string();
    }
    bool le = false, be = false;
    size_t start = 0;
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) le = true, start = 2;
    else if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFE && (unsigned char)raw[1] == 0xFF) be = true, start = 2;
    else if (raw.size() >= 4) {
        // no byte order mark: text with a zero byte in every other position is UTF-16
        size_t zerosOdd = 0, zerosEven = 0, n = std::min<size_t>(raw.size(), 200);
        for (size_t i = 0; i < n; i++)
            if (raw[i] == 0) (i % 2 ? zerosOdd : zerosEven)++;
        if (zerosOdd > n / 4) le = true;
        else if (zerosEven > n / 4) be = true;
    }
    if (!le && !be) {
        if (raw.find('\0') != std::string::npos) {
            problem = "The file is not plain text. Save it as plain text (.txt).";
            return std::string();
        }
        return raw;
    }
    std::string out;
    for (size_t i = start; i + 1 < raw.size(); i += 2) {
        const unsigned lo = (unsigned char)raw[i + (le ? 0 : 1)], hi = (unsigned char)raw[i + (le ? 1 : 0)];
        const unsigned c = hi << 8 | lo;
        if (c < 0x80) out.push_back((char)c);
        else if (c < 0x800) {
            out.push_back((char)(0xC0 | (c >> 6)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        } else if (c < 0xD800 || c > 0xDFFF) {
            out.push_back((char)(0xE0 | (c >> 12)));
            out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        } else {
            out.push_back('?');  // outside the BMP: not needed for links
        }
    }
    return out;
}

std::string leftover(const std::vector<std::string>& lines, const std::string& why) {
    std::string out = "# " + why + "\n# One calendar per line: an optional name, then the https:// or webcal:// link.\n";
    for (auto& l : lines) out += l + "\n";
    return out;
}

}  // namespace calcfg
