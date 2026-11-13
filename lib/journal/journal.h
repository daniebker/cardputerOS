// Journal file format: one Markdown file per day,
//
//     # 2026-11-12
//
//     - **09:15** First entry of the day
//     - **14:32** Another entry
//
// Parsing keeps everything it does not understand, so files edited elsewhere
// survive, and two versions of a day can be merged without losing entries.
#pragma once
#include <string>
#include <vector>

namespace journal {

struct Entry {
    int minutes = 0;   // minutes after midnight
    std::string text;  // without the time marker
};

struct Day {
    std::string preamble;             // everything before the first entry (the "# date" title)
    std::vector<std::string> blocks;  // raw entries, each starting with "- **HH:MM** "
};

Day parse(const std::string& text);
std::string format(const Day& day);

// The entries of a day file, for display.
std::vector<Entry> entries(const std::string& fileText);

// Adds an entry (kept in time order) and returns the new file text. `fileText`
// may be empty for a new day. Line breaks in `text` become spaces.
std::string addEntry(const std::string& fileText, const std::string& date, int minutes, const std::string& text);

// Removes the last entry. Returns false if there is none.
bool removeLastEntry(std::string& fileText);

// Union of the entries of two versions of the same day, in time order.
std::string merge(const std::string& local, const std::string& remote);

// "2026-11-12" style date?
bool validDate(const std::string& date);

}  // namespace journal
