// The calendar import file on the SD card (/cardputerOS/calendar/calendars.txt):
// a plain text file, so long calendar links can be added on a computer instead of
// being typed on the Cardputer. One calendar per line, an optional name followed
// by the link; empty lines and lines starting with # are ignored:
//
//     # name (optional)   link
//     Work       https://calendar.google.com/calendar/ical/.../basic.ics
//     Holidays:  webcal://example.com/holidays.ics
//     https://example.com/team.ics
#pragma once
#include <string>
#include <vector>

namespace calcfg {

struct Line {
    std::string name;  // may be empty
    std::string url;   // as written (https://, http://, webcal:// or webcals://)
};

struct File {
    std::vector<Line> calendars;
    std::vector<std::string> unreadable;  // non-empty lines without a link
};

File parse(const std::string& text);

// True for names the import file may have been given by mistake or by an editor:
// calendars.txt, Calendar.txt, calendars.txt.txt (hidden extension on Windows)...
// Not for macOS's "._" companion files.
bool isImportName(const std::string& fileName);

// The file's bytes as UTF-8 text: UTF-16 files (Notepad's "Unicode") are
// converted. Sets `problem` (and returns "") for formats that are not plain text.
std::string decode(const std::string& raw, std::string& problem);

// Text for the file when some lines could not be imported: an explanation and
// those lines, so they can be fixed on a computer.
std::string leftover(const std::vector<std::string>& lines, const std::string& why);

}  // namespace calcfg
