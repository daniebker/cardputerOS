// The parts of Mindwtr's sync file (data.json) that the Tasks app works with:
// task summaries, field-level edits ("patches") that are replayed on top of the
// latest server copy, and date helpers. Portable, unit tested on the host.
//
// Mindwtr merges data.json per task: the higher `rev` wins, then the later
// `updatedAt`. Every edit made here therefore bumps rev, sets revBy to this
// device and updatedAt to the time of the edit. Deletions are soft (deletedAt).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "mw_json.h"

namespace mindwtr {

// Task statuses in the order they are offered.
extern const char* const kStatuses[];
constexpr int kStatusCount = 7;  // inbox next waiting someday reference done archived
bool validStatus(const std::string& s);
bool finished(const std::string& status);  // done or archived

struct Task {
    std::string id, title, status, dueDate, startTime, projectId, completedAt, description;
    std::vector<std::string> contexts, tags;
    bool deleted = false;    // deletedAt is set
    bool recurring = false;  // has a recurrence rule
    int checklistTotal = 0, checklistDone = 0;
};

// The little the task list needs, kept in memory for every task.
struct TaskRow {
    std::string id, title, status, dueDate;
    int64_t completedKey = 0;  // completedAt in minutes since 1970 (for sorting), 0 = none
    bool deleted = false, recurring = false;
    // byte range of the task in the cached data.json; both 0 for tasks made here
    uint32_t start = 0, end = 0;
};

struct Project {
    std::string id, title, status;
    bool deleted = false;
};

bool summarize(const json::Value& obj, Task& out);
bool summarize(const json::Value& obj, Project& out);
// Titles longer than maxTitle bytes are cut (at a character boundary).
bool summarize(const json::Value& obj, TaskRow& out, size_t maxTitle = 160);

// A pending change to one task. `set` is an object of fields to write, `unset`
// lists fields to remove. A new task carries all its fields in `set`.
struct Patch {
    std::string id;
    bool isNew = false;
    json::Value set = json::Value::object();
    std::vector<std::string> unset;

    void setField(const std::string& key, json::Value v);
    void unsetField(const std::string& key);
};

// Applies the patch's fields (not the revision bookkeeping).
void applyFields(json::Value& task, const Patch& p);
// Applies the patch and the bookkeeping for upload: rev + 1 (1 for new tasks),
// revBy = deviceId. updatedAt comes from the patch.
void applyForUpload(json::Value& task, const Patch& p, const std::string& deviceId);

// Patches are kept on the device as a JSON array between syncs.
std::string serializePatches(const std::vector<Patch>& patches);
bool parsePatches(const std::string& text, std::vector<Patch>& out);

// Field changes for a status change of `task` (as it is now), like Mindwtr does
// it: done sets completedAt, reopening clears it, archiving sets archivedAt.
void changeStatus(Patch& p, const json::Value& task, const std::string& status, const std::string& nowIso);
// A new task in `status` (inbox if empty).
Patch newTask(const std::string& id, const std::string& title, const std::string& status, const std::string& nowIso);

// ---------------------------------------------------------------- dates
struct Date {
    int y = 0, m = 0, d = 0;
};
long daysFromCivil(int y, int m, int d);  // days since 1970-01-01
Date civilFromDays(long days);
int weekday(const Date& d);  // 0 = Sunday

// "2026-11-26" or "2026-11-26T14:30..." -> date; false if not a date.
bool parseDay(const std::string& s, Date& out);
// "HH:MM" part of a date with time, or "".
std::string timeOf(const std::string& s);

// Parses what the user typed for a due / start date. `out` becomes a Mindwtr
// date ("2026-11-30", or "2026-11-30T14:30" with a time; "" clears the date).
// Accepts: empty or "-" (no date), today/tod, tomorrow/tom, yesterday, +3 / +3d /
// +2w / +1m, a weekday name (mon, tuesday, ...: the next one), next week,
// 2026-11-30, 11-30 or 30.11 (this year, or next if already past),
// each optionally followed by a time "14:30" / "9:05" / "14".
bool parseDateInput(const std::string& input, const Date& today, std::string& out);

// Short label relative to today: "today", "tomorrow", "yesterday", "Mon 30 Nov",
// "30 Nov 2027" (other years), with " 14:30" when the date has a time.
std::string dateLabel(const std::string& date, const Date& today);
// Minutes since 1970 of a date or timestamp ("2026-11-01T10:00:00.000Z"), 0 if none.
int64_t minuteKey(const std::string& timestamp);
// Days from today to the date (negative: in the past). False if not a date.
bool daysUntil(const std::string& date, const Date& today, long& out);

// "2026-11-26T09:05:03.000Z" for a Unix time (UTC).
std::string isoTimestamp(int64_t unixSeconds);
// RFC 4122 version 4 UUID from 16 random bytes.
std::string uuidFromBytes(const uint8_t bytes[16]);

}  // namespace mindwtr
