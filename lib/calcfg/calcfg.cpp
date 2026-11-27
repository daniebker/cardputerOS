#include "calcfg.h"

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

std::string leftover(const std::vector<std::string>& lines, const std::string& why) {
    std::string out = "# " + why + "\n# One calendar per line: an optional name, then the https:// or webcal:// link.\n";
    for (auto& l : lines) out += l + "\n";
    return out;
}

}  // namespace calcfg
