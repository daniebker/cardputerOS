// iCalendar (.ics, RFC 5545) reader for subscribed calendars: a streaming parser
// that keeps only what an agenda needs, and expansion of events (including
// repeating ones) into occurrences in a time window.
//
// Supported: VEVENT with DTSTART / DTEND / DURATION, all-day events, UTC,
// floating and TZID times (resolved with the VTIMEZONE definitions in the file),
// RRULE (DAILY, WEEKLY, MONTHLY, YEARLY with INTERVAL, COUNT, UNTIL, BYDAY,
// BYMONTHDAY, BYMONTH, BYSETPOS, WKST), RDATE, EXDATE, RECURRENCE-ID overrides
// and STATUS:CANCELLED. Lines are unfolded, text is unescaped. Not supported:
// BYWEEKNO / BYYEARDAY / BYHOUR and finer rules (ignored).
//
// Memory is bounded: text fields are cut to `maxText`, overlong lines are cut,
// one-off events outside [keepAfter, keepBefore) are dropped while parsing, and
// no more events are kept once they use about `maxBytes`.
#pragma once
#include <cstddef>
#include <climits>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ics {

// ---------------------------------------------------------------- dates
int64_t daysFromCivil(int y, int m, int d);  // days since 1970-01-01
void civilFromDays(int64_t days, int& y, int& m, int& d);
int weekday(int64_t days);  // 0 = Sunday
int daysInMonth(int y, int m);

struct DateTime {
    int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
    bool valid = false;
    bool dateOnly = false;  // VALUE=DATE (all-day)
    bool utc = false;       // ends in Z
    std::string tzid;       // empty: floating (device local time)
    // The wall-clock time as seconds since 1970 (as if it were UTC).
    int64_t wall() const;
};
// "20261127", "20261127T093000", "20261127T093000Z".
bool parseDateTime(const std::string& value, DateTime& out);
// "PT1H30M", "P1D", "-P1W" -> seconds.
bool parseDuration(const std::string& value, int64_t& seconds);
// "+0100", "-0530" -> seconds east of UTC.
bool parseOffset(const std::string& value, int& seconds);
std::string unescapeText(const std::string& value);

// ---------------------------------------------------------------- RRULE
struct Rule {
    enum Freq { None, Daily, Weekly, Monthly, Yearly };
    Freq freq = None;
    int interval = 1;
    int count = 0;  // 0: no limit
    DateTime until;  // !valid: no limit
    std::vector<std::pair<int, int>> byDay;  // (ordinal, 0 = every; weekday 0 = Sunday)
    std::vector<int> byMonthDay, byMonth, bySetPos;
    int wkst = 1;  // Monday
};
bool parseRule(const std::string& value, Rule& out);

// Calls fn(wallSeconds) for each start of the rule in order, beginning with
// `start` itself (if it matches the rule), until fn returns false, COUNT is
// reached or the next start is later than `stopAfterWall`. UNTIL is left to
// the caller (it needs the time zone).
template <typename Fn>
void expandRule(const DateTime& start, const Rule& r, int64_t stopAfterWall, Fn fn);

// ---------------------------------------------------------------- calendar
struct Event {
    std::string uid, summary, location, description;
    DateTime start, end, recurrenceId;
    int64_t duration = -1;  // from DURATION; -1 = none
    Rule rule;
    std::vector<DateTime> exdates, rdates;
    bool cancelled = false;
};

struct Occurrence {
    int64_t start = 0, end = 0;  // UTC seconds
    bool allDay = false;
    uint32_t event = 0;  // index into Calendar::events()
};

class Calendar {
public:
    // Limits, set before feeding.
    size_t maxText = 400;       // bytes kept of SUMMARY / LOCATION / DESCRIPTION
    size_t maxEvents = 3000;    // events kept (later ones are dropped, see truncated())
    size_t maxBytes = SIZE_MAX; // memory the kept events may use (approximately)
    int64_t keepAfter = INT64_MIN;  // drop one-off events that ended before (UTC seconds)
    int64_t keepBefore = INT64_MAX; // drop one-off events that start after (UTC seconds)

    void feed(const char* data, size_t len);
    void finish();  // after the last feed()

    bool sawCalendar() const { return sawCalendar_; }  // BEGIN:VCALENDAR was seen
    bool truncated() const { return truncated_; }
    size_t bytesUsed() const { return bytes_; }
    const std::string& name() const { return name_; }  // X-WR-CALNAME
    const std::vector<Event>& events() const { return events_; }

    // Occurrences overlapping [from, to) (UTC seconds), sorted by start; at most
    // maxCount (the earliest). Floating and all-day times use `localOffset`
    // (seconds east of UTC, the device's time zone).
    std::vector<Occurrence> expand(int64_t from, int64_t to, int localOffset, size_t maxCount) const;

    // UTC seconds of a date-time from this file.
    int64_t toUtc(const DateTime& dt, int localOffset) const;

private:
    struct Observance {
        DateTime start;  // local time, in offsetFrom
        int offsetFrom = 0, offsetTo = 0;
        Rule rule;
        std::vector<DateTime> rdates;
    };
    struct Zone {
        std::vector<Observance> obs;
    };

    void physicalLine(std::string& line);
    void logicalLine(const std::string& line);
    void endEvent();
    int zoneOffset(const std::string& id, const Zone& z, int64_t wall) const;
    // The offset at `wall` from every onset of every observance (slow when rules
    // start long ago: used once per zone and year).
    int slowOffset(const Zone& z, int64_t wall) const;
    static size_t footprint(const Event& e);
    const std::string& top() const;

    std::vector<Event> events_;
    std::map<std::string, Zone> zones_;
    std::vector<std::string> stack_;
    Event ev_;
    Zone zone_;
    std::string zoneId_;
    Observance obs_;
    std::string name_, raw_, logical_;
    bool sawCalendar_ = false, truncated_ = false;
    size_t bytes_ = 0;
    // zone transitions by year: "tzid\tyear" -> (onset wall time, offset after it), sorted
    mutable std::map<std::string, std::vector<std::pair<int64_t, int>>> transitions_;
};

// ---------------------------------------------------------------- implementation of expandRule

namespace detail {
// Days of month (y, m) selected by the BYMONTHDAY / BYDAY / BYSETPOS parts, or
// `defaultDay`; sorted.
std::vector<int> monthDays(int y, int m, const Rule& r, int defaultDay);
bool inList(const std::vector<int>& v, int x);
}  // namespace detail

template <typename Fn>
void expandRule(const DateTime& start, const Rule& r, int64_t stopAfterWall, Fn fn) {
    const int64_t startDays = daysFromCivil(start.y, start.m, start.d);
    const int64_t tod = (int64_t)start.hh * 3600 + start.mm * 60 + start.ss;
    const int64_t startWall = startDays * 86400 + tod;
    const int interval = r.interval > 0 ? r.interval : 1;
    int emitted = 0;
    constexpr long kMaxPeriods = 200000;
    // returns false to stop
    auto emit = [&](int64_t day) -> bool {
        const int64_t w = day * 86400 + tod;
        if (w < startWall) return true;
        if (w > stopAfterWall) return false;
        if (!fn(w)) return false;
        emitted++;
        return !(r.count > 0 && emitted >= r.count);
    };
    if (r.freq == Rule::None) {
        if (startWall <= stopAfterWall) fn(startWall);
        return;
    }
    if (r.freq == Rule::Daily) {
        for (long k = 0; k < kMaxPeriods; k++) {
            const int64_t day = startDays + (int64_t)k * interval;
            if (day * 86400 > stopAfterWall) return;
            int y, m, d;
            civilFromDays(day, y, m, d);
            if (!r.byMonth.empty() && !detail::inList(r.byMonth, m)) continue;
            if (!r.byMonthDay.empty() && !detail::inList(r.byMonthDay, d) &&
                !detail::inList(r.byMonthDay, d - daysInMonth(y, m) - 1))
                continue;
            if (!r.byDay.empty()) {
                bool ok = false;
                for (auto& bd : r.byDay) ok = ok || bd.second == weekday(day);
                if (!ok) continue;
            }
            if (!emit(day)) return;
        }
        return;
    }
    if (r.freq == Rule::Weekly) {
        const int wkst = r.wkst;
        const int64_t weekStart = startDays - ((weekday(startDays) - wkst + 7) % 7);
        std::vector<int> offsets;
        if (r.byDay.empty()) offsets.push_back((weekday(startDays) - wkst + 7) % 7);
        for (auto& bd : r.byDay) {
            const int o = (bd.second - wkst + 7) % 7;
            if (!detail::inList(offsets, o)) offsets.push_back(o);
        }
        for (size_t i = 1; i < offsets.size(); i++)  // sort (tiny)
            for (size_t j = i; j > 0 && offsets[j] < offsets[j - 1]; j--) std::swap(offsets[j], offsets[j - 1]);
        for (long k = 0; k < kMaxPeriods; k++) {
            const int64_t wk = weekStart + (int64_t)k * 7 * interval;
            if (wk * 86400 > stopAfterWall) return;
            for (int o : offsets) {
                const int64_t day = wk + o;
                if (!r.byMonth.empty()) {
                    int y, m, d;
                    civilFromDays(day, y, m, d);
                    if (!detail::inList(r.byMonth, m)) continue;
                }
                if (!emit(day)) return;
            }
        }
        return;
    }
    if (r.freq == Rule::Monthly) {
        for (long k = 0; k < kMaxPeriods; k++) {
            const long mi = (long)start.y * 12 + (start.m - 1) + k * interval;
            const int y = (int)(mi / 12), m = (int)(mi % 12) + 1;
            if (daysFromCivil(y, m, 1) * 86400 > stopAfterWall) return;
            if (!r.byMonth.empty() && !detail::inList(r.byMonth, m)) continue;
            for (int d : detail::monthDays(y, m, r, start.d))
                if (!emit(daysFromCivil(y, m, d))) return;
        }
        return;
    }
    // Yearly
    for (long k = 0; k < kMaxPeriods; k++) {
        const int y = start.y + (int)(k * interval);
        if (daysFromCivil(y, 1, 1) * 86400 > stopAfterWall) return;
        std::vector<int> months = r.byMonth;
        const bool everyMonth = months.empty() && (!r.byDay.empty() || !r.byMonthDay.empty());
        if (months.empty()) {
            if (everyMonth)
                for (int m = 1; m <= 12; m++) months.push_back(m);
            else
                months.push_back(start.m);
        }
        for (size_t i = 1; i < months.size(); i++)
            for (size_t j = i; j > 0 && months[j] < months[j - 1]; j--) std::swap(months[j], months[j - 1]);
        for (int m : months) {
            if (m < 1 || m > 12) continue;
            for (int d : detail::monthDays(y, m, r, start.d))
                if (!emit(daysFromCivil(y, m, d))) return;
        }
    }
}

}  // namespace ics
