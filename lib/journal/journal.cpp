#include "journal.h"

#include <algorithm>
#include <cstdio>

namespace journal {

namespace {
bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Does the line start an entry ("- **HH:MM**")? Returns the minutes, or -1.
int entryStart(const std::string& line) {
    if (line.size() < 11 || line.compare(0, 4, "- **") != 0) return -1;
    if (!isDigit(line[4]) || !isDigit(line[5]) || line[6] != ':' || !isDigit(line[7]) || !isDigit(line[8])) return -1;
    if (line.compare(9, 2, "**") != 0) return -1;
    const int h = (line[4] - '0') * 10 + (line[5] - '0'), m = (line[7] - '0') * 10 + (line[8] - '0');
    return (h < 24 && m < 60) ? h * 60 + m : -1;
}

std::string rtrim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string l = text.substr(pos, nl - pos);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
        pos = nl + 1;
    }
    return out;
}
}  // namespace

bool validDate(const std::string& d) {
    if (d.size() != 10 || d[4] != '-' || d[7] != '-') return false;
    for (int i : {0, 1, 2, 3, 5, 6, 8, 9})
        if (!isDigit(d[i])) return false;
    const int m = (d[5] - '0') * 10 + (d[6] - '0'), day = (d[8] - '0') * 10 + (d[9] - '0');
    return m >= 1 && m <= 12 && day >= 1 && day <= 31;
}

Day parse(const std::string& text) {
    Day day;
    bool inBlocks = false;
    for (const std::string& l : lines(text)) {
        if (entryStart(l) >= 0) {
            inBlocks = true;
            day.blocks.push_back(l);
        } else if (inBlocks) {
            day.blocks.back() += "\n" + l;  // continuation of the entry above
        } else {
            day.preamble += l + "\n";
        }
    }
    day.preamble = rtrim(day.preamble);
    for (auto& b : day.blocks) b = rtrim(b);
    return day;
}

std::string format(const Day& day) {
    std::string out = day.preamble;
    if (!out.empty()) out += "\n\n";
    for (auto& b : day.blocks) out += b + "\n";
    return out;
}

std::vector<Entry> entries(const std::string& fileText) {
    std::vector<Entry> out;
    for (auto& b : parse(fileText).blocks) {
        Entry e;
        e.minutes = entryStart(b.substr(0, b.find('\n')));
        e.text = b.size() > 11 ? b.substr(11) : std::string();
        if (!e.text.empty() && e.text[0] == ' ') e.text.erase(0, 1);
        out.push_back(e);
    }
    return out;
}

std::string addEntry(const std::string& fileText, const std::string& date, int minutes, const std::string& text) {
    Day day = parse(fileText);
    if (day.preamble.empty() && day.blocks.empty()) day.preamble = "# " + date;
    std::string clean;
    for (char c : text) clean.push_back((c == '\n' || c == '\r' || c == '\t') ? ' ' : c);
    clean = rtrim(clean);
    char head[16];
    snprintf(head, sizeof head, "- **%02d:%02d** ", (minutes / 60) % 24, minutes % 60);
    // after the last entry that is not later than the new one
    size_t pos = day.blocks.size();
    while (pos > 0 && entryStart(day.blocks[pos - 1].substr(0, day.blocks[pos - 1].find('\n'))) > minutes) pos--;
    day.blocks.insert(day.blocks.begin() + pos, head + clean);
    return format(day);
}

bool removeLastEntry(std::string& fileText) {
    Day day = parse(fileText);
    if (day.blocks.empty()) return false;
    day.blocks.pop_back();
    fileText = format(day);
    return true;
}

std::string merge(const std::string& local, const std::string& remote) {
    const Day a = parse(remote), b = parse(local);
    Day out;
    out.preamble = a.preamble.empty() ? b.preamble : a.preamble;
    out.blocks = a.blocks;
    for (auto& blk : b.blocks)
        if (std::find(out.blocks.begin(), out.blocks.end(), blk) == out.blocks.end()) out.blocks.push_back(blk);
    std::stable_sort(out.blocks.begin(), out.blocks.end(), [](const std::string& x, const std::string& y) {
        return entryStart(x.substr(0, x.find('\n'))) < entryStart(y.substr(0, y.find('\n')));
    });
    return format(out);
}

}  // namespace journal
