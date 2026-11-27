// Calendar subscriptions and their local copies.
//
// The list of subscriptions is a setting on the device ("cal_list"); the links
// often carry a private token, so they are stored encrypted like passwords.
// Each calendar's last download is kept on the microSD card as it came from the
// server, with a small file of cache metadata next to it:
//
//     /cardputerOS/calendar/<id>.ics    the calendar
//     /cardputerOS/calendar/<id>.meta   ETag, Last-Modified, when it was checked
//     /cardputerOS/calendar/<id>.tmp    a download in progress
//     /cardputerOS/calendar/<id>.occ    the agenda index built from it (see lib/calidx)
//     /cardputerOS/calendar/<id>.txt    the texts of the indexed events
//
// The .ics is only replaced once a download has completed and looks like a
// calendar, so a failed refresh always leaves the previous copy usable.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct CalendarSub {
    int id = 0;
    std::string name;
    int color = 0;  // index into calstore::kColors
    // secret / settings
    std::string url, user, pass;
    // cache metadata (on the card)
    std::string etag, lastModified, error, feedName;
    int64_t checkedAt = 0;  // last time the server confirmed or sent a copy (UTC seconds; 0 = never)
    int64_t fetchedAt = 0;  // last time a new copy was downloaded
    bool cached = false;    // <id>.ics exists
    std::string note;       // a problem reading the copy (e.g. too large), not saved
    // the agenda index (<id>.occ / .txt): the window it covers (local days), the
    // time zone and the download it was built for
    int64_t indexFrom = 0, indexTo = 0, indexFetched = -1;
    int indexOffset = 0;
    uint32_t indexRecords = 0;
    bool indexTruncated = false;

    std::string title() const { return !name.empty() ? name : !feedName.empty() ? feedName : "Calendar " + std::to_string(id); }
};

namespace calstore {

extern const char* kDir;
constexpr int kMaxCalendars = 8;
constexpr int kColorCount = 6;
extern const uint32_t kColors[kColorCount];
extern const char* const kColorNames[kColorCount];

std::vector<CalendarSub> loadSubs();  // settings and secrets (not the card)
void saveSubs(const std::vector<CalendarSub>& subs);
// Blanks the link and login of a removed calendar.
void forgetSecrets(int id);
int newId(const std::vector<CalendarSub>& subs);

std::string icsPath(int id);
std::string tmpPath(int id);
std::string occPath(int id);
std::string txtPath(int id);
// The following need an open hal::SdSession.
void readMeta(CalendarSub& s);
bool writeMeta(const CalendarSub& s);
// Makes the finished download the calendar's copy.
bool commitDownload(int id);
void removeFiles(int id);

}  // namespace calstore
