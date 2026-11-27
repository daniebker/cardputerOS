#include "ics.h"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace ics {

namespace {
constexpr size_t kMaxLine = 4096;  // longer (unfolded) lines are cut
constexpr int64_t kDay = 86400;

std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    return s;
}

int digits(const std::string& s, size_t pos, size_t n) {
    int v = 0;
    for (size_t i = pos; i < pos + n; i++) {
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

// Cuts `s` to at most `max` bytes without splitting a UTF-8 character.
void cutUtf8(std::string& s, size_t max) {
    if (s.size() <= max) return;
    size_t n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    s.resize(n);
}

std::vector<std::string> splitList(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find(sep, pos);
        if (e == std::string::npos) e = s.size();
        if (e > pos) out.push_back(s.substr(pos, e - pos));
        pos = e + 1;
    }
    return out;
}

int weekdayCode(const std::string& s) {
    static const char* const kDays[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
    for (int i = 0; i < 7; i++)
        if (s == kDays[i]) return i;
    return -1;
}

bool isUtcName(const std::string& tzid) {
    const std::string u = upper(tzid);
    return u == "UTC" || u == "GMT" || u == "Z" || u == "ETC/UTC" || u == "ETC/GMT" || u == "UTC+0" ||
           u == "GREENWICH STANDARD TIME" || u == "COORDINATED UNIVERSAL TIME";
}
}  // namespace

// ---------------------------------------------------------------- dates

// Howard Hinnant's civil calendar algorithms.
int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civilFromDays(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yoe + era * 400 + (m <= 2));
}

int weekday(int64_t days) {
    const int64_t w = (days + 4) % 7;  // 1970-01-01 was a Thursday
    return (int)(w < 0 ? w + 7 : w);
}

int daysInMonth(int y, int m) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2) return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 29 : 28;
    return m >= 1 && m <= 12 ? kDays[m - 1] : 30;
}

int64_t DateTime::wall() const { return daysFromCivil(y, m, d) * kDay + (int64_t)hh * 3600 + mm * 60 + ss; }

bool parseDateTime(const std::string& value, DateTime& out) {
    out = DateTime();
    std::string v = value;
    while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
    if (v.size() < 8) return false;
    out.y = digits(v, 0, 4);
    out.m = digits(v, 4, 2);
    out.d = digits(v, 6, 2);
    if (out.y < 0 || out.m < 1 || out.m > 12 || out.d < 1 || out.d > daysInMonth(out.y, out.m)) return false;
    if (v.size() == 8) {
        out.dateOnly = true;
        out.valid = true;
        return true;
    }
    if (v.size() < 15 || (v[8] != 'T' && v[8] != 't')) return false;
    out.hh = digits(v, 9, 2);
    out.mm = digits(v, 11, 2);
    out.ss = digits(v, 13, 2);
    if (out.hh < 0 || out.hh > 23 || out.mm < 0 || out.mm > 59 || out.ss < 0 || out.ss > 60) return false;
    if (out.ss == 60) out.ss = 59;  // leap second
    out.utc = v.size() > 15 && (v[15] == 'Z' || v[15] == 'z');
    out.valid = true;
    return true;
}

bool parseDuration(const std::string& value, int64_t& seconds) {
    size_t i = 0;
    int sign = 1;
    if (i < value.size() && (value[i] == '+' || value[i] == '-')) sign = value[i++] == '-' ? -1 : 1;
    if (i >= value.size() || (value[i] != 'P' && value[i] != 'p')) return false;
    i++;
    int64_t total = 0, num = 0;
    bool haveNum = false, any = false;
    for (; i < value.size(); i++) {
        const char c = (char)toupper((unsigned char)value[i]);
        if (c >= '0' && c <= '9') {
            num = num * 10 + (c - '0');
            haveNum = true;
            continue;
        }
        if (c == 'T') continue;
        if (!haveNum) return false;
        int64_t unit;
        switch (c) {
            case 'W': unit = 7 * kDay; break;
            case 'D': unit = kDay; break;
            case 'H': unit = 3600; break;
            case 'M': unit = 60; break;
            case 'S': unit = 1; break;
            default: return false;
        }
        total += num * unit;
        num = 0;
        haveNum = false;
        any = true;
    }
    if (!any || haveNum) return false;
    seconds = sign * total;
    return true;
}

bool parseOffset(const std::string& value, int& seconds) {
    if (value.size() < 5 || (value[0] != '+' && value[0] != '-')) return false;
    const int h = digits(value, 1, 2), m = digits(value, 3, 2);
    int s = 0;
    if (value.size() >= 7) s = digits(value, 5, 2);
    if (h < 0 || m < 0 || s < 0) return false;
    seconds = (value[0] == '-' ? -1 : 1) * (h * 3600 + m * 60 + s);
    return true;
}

std::string unescapeText(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); i++) {
        const char c = value[i];
        if (c == '\\' && i + 1 < value.size()) {
            const char n = value[++i];
            if (n == 'n' || n == 'N') out.push_back('\n');
            else out.push_back(n);  // \, \; \\ and anything else
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// ---------------------------------------------------------------- RRULE

bool parseRule(const std::string& value, Rule& out) {
    out = Rule();
    for (const std::string& part : splitList(value, ';')) {
        const size_t eq = part.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = upper(part.substr(0, eq)), v = upper(part.substr(eq + 1));
        if (k == "FREQ") {
            if (v == "DAILY") out.freq = Rule::Daily;
            else if (v == "WEEKLY") out.freq = Rule::Weekly;
            else if (v == "MONTHLY") out.freq = Rule::Monthly;
            else if (v == "YEARLY") out.freq = Rule::Yearly;
            else return false;  // SECONDLY..HOURLY: not shown
        } else if (k == "INTERVAL") {
            out.interval = std::max(1, atoi(v.c_str()));
        } else if (k == "COUNT") {
            out.count = std::max(0, atoi(v.c_str()));
        } else if (k == "UNTIL") {
            parseDateTime(v, out.until);
        } else if (k == "BYDAY") {
            for (const std::string& d : splitList(v, ',')) {
                if (d.size() < 2) continue;
                const int wd = weekdayCode(d.substr(d.size() - 2));
                if (wd < 0) continue;
                const int ord = d.size() > 2 ? atoi(d.substr(0, d.size() - 2).c_str()) : 0;
                out.byDay.push_back({ord, wd});
            }
        } else if (k == "BYMONTHDAY" || k == "BYMONTH" || k == "BYSETPOS") {
            std::vector<int>& list = k == "BYMONTHDAY" ? out.byMonthDay : k == "BYMONTH" ? out.byMonth : out.bySetPos;
            for (const std::string& n : splitList(v, ',')) {
                const int x = atoi(n.c_str());
                if (x != 0) list.push_back(x);
            }
        } else if (k == "WKST") {
            const int wd = weekdayCode(v);
            if (wd >= 0) out.wkst = wd;
        }
    }
    return out.freq != Rule::None;
}

namespace detail {

bool inList(const std::vector<int>& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); }

std::vector<int> monthDays(int y, int m, const Rule& r, int defaultDay) {
    const int dim = daysInMonth(y, m);
    std::vector<int> days;
    if (r.byMonthDay.empty() && r.byDay.empty()) {
        if (defaultDay <= dim) days.push_back(defaultDay);
        return days;
    }
    const int firstWd = weekday(daysFromCivil(y, m, 1));
    std::vector<bool> on(dim + 1, false);
    bool any = false;
    if (!r.byDay.empty()) {
        for (auto& bd : r.byDay) {
            const int first = 1 + (bd.second - firstWd + 7) % 7;  // first such weekday in the month
            std::vector<int> all;
            for (int d = first; d <= dim; d += 7) all.push_back(d);
            if (bd.first == 0) {
                for (int d : all) on[d] = true;
            } else {
                const int idx = bd.first > 0 ? bd.first - 1 : (int)all.size() + bd.first;
                if (idx >= 0 && idx < (int)all.size()) on[all[idx]] = true;
            }
        }
        any = true;
    }
    if (!r.byMonthDay.empty()) {
        std::vector<bool> md(dim + 1, false);
        for (int x : r.byMonthDay) {
            const int d = x > 0 ? x : dim + x + 1;
            if (d >= 1 && d <= dim) md[d] = true;
        }
        for (int d = 1; d <= dim; d++) on[d] = any ? (on[d] && md[d]) : md[d];
    }
    for (int d = 1; d <= dim; d++)
        if (on[d]) days.push_back(d);
    if (!r.bySetPos.empty()) {
        std::vector<int> picked;
        for (int p : r.bySetPos) {
            const int idx = p > 0 ? p - 1 : (int)days.size() + p;
            if (idx >= 0 && idx < (int)days.size() && !inList(picked, days[idx])) picked.push_back(days[idx]);
        }
        std::sort(picked.begin(), picked.end());
        days.swap(picked);
    }
    return days;
}

}  // namespace detail

// ---------------------------------------------------------------- parser

const std::string& Calendar::top() const {
    static const std::string none;
    return stack_.empty() ? none : stack_.back();
}

void Calendar::feed(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        const char c = data[i];
        if (c == '\n') {
            physicalLine(raw_);
            raw_.clear();
        } else if (raw_.size() < kMaxLine) {
            raw_.push_back(c);
        }
    }
}

void Calendar::finish() {
    if (!raw_.empty()) physicalLine(raw_);
    raw_.clear();
    if (!logical_.empty()) logicalLine(logical_);
    logical_.clear();
    std::string().swap(raw_);
    std::string().swap(logical_);
}

void Calendar::physicalLine(std::string& line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {  // folded continuation
        if (logical_.size() < kMaxLine) logical_.append(line, 1, kMaxLine - logical_.size());
        return;
    }
    if (!logical_.empty()) logicalLine(logical_);
    logical_ = line;
}

void Calendar::logicalLine(const std::string& line) {
    // NAME;PARAM=a;PARAM="b:c":value
    size_t colon = std::string::npos;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == '"') quoted = !quoted;
        else if (line[i] == ':' && !quoted) {
            colon = i;
            break;
        }
    }
    if (colon == std::string::npos) return;
    const std::string head = line.substr(0, colon), value = line.substr(colon + 1);
    const size_t semi = head.find(';');
    const std::string name = upper(head.substr(0, semi));
    std::string tzid;
    bool dateParam = false;
    if (semi != std::string::npos) {
        for (const std::string& p : splitList(head.substr(semi + 1), ';')) {
            const size_t eq = p.find('=');
            if (eq == std::string::npos) continue;
            const std::string pn = upper(p.substr(0, eq));
            std::string pv = p.substr(eq + 1);
            if (pv.size() >= 2 && pv.front() == '"' && pv.back() == '"') pv = pv.substr(1, pv.size() - 2);
            if (pn == "TZID") tzid = pv;
            else if (pn == "VALUE" && upper(pv) == "DATE") dateParam = true;
        }
    }

    if (name == "BEGIN") {
        const std::string comp = upper(value);
        if (comp == "VCALENDAR") sawCalendar_ = true;
        else if (comp == "VEVENT" && top() == "VCALENDAR") ev_ = Event();
        else if (comp == "VTIMEZONE") {
            zone_ = Zone();
            zoneId_.clear();
        } else if ((comp == "STANDARD" || comp == "DAYLIGHT") && top() == "VTIMEZONE") {
            obs_ = Observance();
        }
        if (stack_.size() < 16) stack_.push_back(comp);
        return;
    }
    if (name == "END") {
        const std::string comp = upper(value);
        if (stack_.empty() || stack_.back() != comp) return;  // malformed: ignore
        stack_.pop_back();
        if (comp == "VEVENT" && top() == "VCALENDAR") endEvent();
        else if ((comp == "STANDARD" || comp == "DAYLIGHT") && top() == "VTIMEZONE") {
            if (obs_.start.valid) zone_.obs.push_back(std::move(obs_));
        } else if (comp == "VTIMEZONE" && !zoneId_.empty()) {
            zones_[zoneId_] = std::move(zone_);
        }
        return;
    }

    auto readTime = [&](DateTime& dt) {
        if (!parseDateTime(value, dt)) return false;
        if (dateParam) {
            dt.dateOnly = true;
            dt.hh = dt.mm = dt.ss = 0;
            dt.utc = false;
        }
        if (!dt.utc && !dt.dateOnly) dt.tzid = tzid;
        return true;
    };
    auto readTimes = [&](std::vector<DateTime>& list) {
        for (const std::string& v : splitList(value, ',')) {
            DateTime dt;
            if (!parseDateTime(v, dt)) continue;  // e.g. VALUE=PERIOD
            if (dateParam) dt.dateOnly = true;
            if (!dt.utc && !dt.dateOnly) dt.tzid = tzid;
            if (list.size() < 400) list.push_back(dt);
        }
    };
    auto text = [&](std::string& field) {
        field = unescapeText(value);
        cutUtf8(field, maxText);
    };

    const std::string& t = top();
    if (t == "VEVENT" && stack_.size() >= 2 && stack_[stack_.size() - 2] == "VCALENDAR") {
        if (name == "UID") ev_.uid = value;
        else if (name == "SUMMARY") text(ev_.summary);
        else if (name == "LOCATION") text(ev_.location);
        else if (name == "DESCRIPTION") text(ev_.description);
        else if (name == "DTSTART") readTime(ev_.start);
        else if (name == "DTEND") readTime(ev_.end);
        else if (name == "DURATION") {
            int64_t s;
            if (parseDuration(value, s)) ev_.duration = s;
        } else if (name == "RRULE") parseRule(value, ev_.rule);
        else if (name == "RECURRENCE-ID") readTime(ev_.recurrenceId);
        else if (name == "EXDATE") readTimes(ev_.exdates);
        else if (name == "RDATE") readTimes(ev_.rdates);
        else if (name == "STATUS") ev_.cancelled = upper(value) == "CANCELLED";
    } else if (t == "VTIMEZONE") {
        if (name == "TZID") zoneId_ = value;
    } else if (t == "STANDARD" || t == "DAYLIGHT") {
        if (name == "DTSTART") parseDateTime(value, obs_.start);
        else if (name == "TZOFFSETFROM") parseOffset(value, obs_.offsetFrom);
        else if (name == "TZOFFSETTO") parseOffset(value, obs_.offsetTo);
        else if (name == "RRULE") parseRule(value, obs_.rule);
        else if (name == "RDATE") {
            for (const std::string& v : splitList(value, ',')) {
                DateTime dt;
                if (parseDateTime(v, dt) && obs_.rdates.size() < 64) obs_.rdates.push_back(dt);
            }
        }
    } else if (t == "VCALENDAR") {
        if (name == "X-WR-CALNAME") text(name_);
    }
}

void Calendar::endEvent() {
    Event& e = ev_;
    if (!e.start.valid) return;
    // Drop one-off events that are long over (the window can only be a little off
    // here, as time zones may be defined later in the file: keep two days' slack).
    if (keepAfter != INT64_MIN) {
        const int64_t limit = keepAfter - 2 * kDay;
        int64_t endWall = e.start.wall() + (e.start.dateOnly ? kDay : 0);
        if (e.end.valid) endWall = std::max(endWall, e.end.wall());
        if (e.duration > 0) endWall = std::max(endWall, e.start.wall() + e.duration);
        const bool repeats = e.rule.freq != Rule::None || !e.rdates.empty();
        if (!repeats && endWall < limit &&
            (!e.recurrenceId.valid || e.recurrenceId.wall() + 7 * kDay < limit))
            return;
        if (repeats && e.rule.until.valid && e.rdates.empty() && e.rule.until.wall() + 7 * kDay < limit) return;
    }
    if (events_.size() >= maxEvents) {
        truncated_ = true;
        return;
    }
    events_.push_back(std::move(e));
    ev_ = Event();
}

// ---------------------------------------------------------------- time zones

int Calendar::zoneOffset(const Zone& z, int64_t wall) const {
    // The observance that started last before `wall` (onsets are in local time).
    int64_t best = INT64_MIN;
    int offset = 0;
    bool found = false;
    const Observance* earliest = nullptr;
    for (const Observance& o : z.obs) {
        if (!earliest || o.start.wall() < earliest->start.wall()) earliest = &o;
        int64_t onset = INT64_MIN;
        auto consider = [&](int64_t w) {
            if (w <= wall && w > onset) onset = w;
        };
        if (o.rule.freq != Rule::None) {
            int64_t until = INT64_MAX;
            if (o.rule.until.valid) until = o.rule.until.wall() + (o.rule.until.utc ? o.offsetFrom : 0);
            expandRule(o.start, o.rule, wall, [&](int64_t w) {
                if (w > until) return false;
                consider(w);
                return true;
            });
        } else {
            consider(o.start.wall());
        }
        for (const DateTime& r : o.rdates) consider(r.wall());
        if (onset != INT64_MIN && onset > best) {
            best = onset;
            offset = o.offsetTo;
            found = true;
        }
    }
    if (!found && earliest) return earliest->offsetFrom;
    return offset;
}

int64_t Calendar::toUtc(const DateTime& dt, int localOffset) const {
    if (!dt.valid) return 0;
    const int64_t w = dt.wall();
    if (dt.utc) return w;
    if (dt.dateOnly || dt.tzid.empty()) return w - localOffset;
    auto it = zones_.find(dt.tzid);
    if (it != zones_.end() && !it->second.obs.empty()) return w - zoneOffset(it->second, w);
    if (isUtcName(dt.tzid)) return w;
    return w - localOffset;  // unknown zone: assume the device's
}

// ---------------------------------------------------------------- expansion

std::vector<Occurrence> Calendar::expand(int64_t from, int64_t to, int localOffset, size_t maxCount) const {
    std::vector<Occurrence> out;
    const size_t hardCap = maxCount * 4 + 64;  // collected before sorting and cutting to maxCount

    // instances replaced by a RECURRENCE-ID override: "uid\tutc"
    std::set<std::string> overridden;
    for (const Event& e : events_)
        if (e.recurrenceId.valid) overridden.insert(e.uid + "\t" + std::to_string(toUtc(e.recurrenceId, localOffset)));

    for (uint32_t i = 0; i < events_.size() && out.size() < hardCap; i++) {
        const Event& e = events_[i];
        if (e.cancelled) continue;
        const bool allDay = e.start.dateOnly;
        const int64_t start = toUtc(e.start, localOffset);
        int64_t dur;
        if (e.end.valid) dur = toUtc(e.end, localOffset) - start;
        else if (e.duration >= 0) dur = e.duration;
        else dur = allDay ? kDay : 0;
        if (dur < 0) dur = 0;

        auto add = [&](int64_t s) {
            const int64_t en = s + dur;
            if (s < to && (en > from || (dur == 0 && s >= from))) out.push_back({s, en, allDay, i});
        };
        if (e.recurrenceId.valid || (e.rule.freq == Rule::None && e.rdates.empty())) {
            add(start);
            continue;
        }

        // repeating
        std::set<int64_t> excluded, excludedDays;
        for (const DateTime& x : e.exdates) {
            if (x.dateOnly) excludedDays.insert(daysFromCivil(x.y, x.m, x.d));
            else excluded.insert(toUtc(x, localOffset));
        }
        const std::string prefix = e.uid + "\t";
        auto instance = [&](int64_t s, int64_t wall) {
            if (excluded.count(s) || excludedDays.count(wall >= 0 ? wall / kDay : (wall - kDay + 1) / kDay)) return;
            if (!overridden.empty() && overridden.count(prefix + std::to_string(s))) return;
            add(s);
        };
        int64_t until = INT64_MAX;
        if (e.rule.until.valid) {
            until = toUtc(e.rule.until, localOffset);
            if (e.rule.until.dateOnly) until += kDay - 1;
        }
        if (e.rule.freq != Rule::None) {
            expandRule(e.start, e.rule, to - localOffset + 2 * kDay, [&](int64_t wall) {
                if (wall + dur + 3 * kDay < from) return true;  // long before the window: skip the time zone work
                DateTime dt = e.start;
                const int64_t days = wall >= 0 ? wall / kDay : (wall - kDay + 1) / kDay;
                civilFromDays(days, dt.y, dt.m, dt.d);
                const int64_t s = toUtc(dt, localOffset);
                if (s > until) return false;
                instance(s, wall);
                return s < to && out.size() < hardCap;
            });
        } else {
            instance(start, e.start.wall());
        }
        for (const DateTime& r : e.rdates) {
            DateTime dt = r;
            if (!dt.utc && !dt.dateOnly && dt.tzid.empty()) dt.tzid = e.start.tzid;
            instance(toUtc(dt, localOffset), dt.wall());
        }
    }
    std::sort(out.begin(), out.end(), [](const Occurrence& a, const Occurrence& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.allDay != b.allDay) return a.allDay;
        return a.event < b.event;
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const Occurrence& a, const Occurrence& b) { return a.start == b.start && a.event == b.event; }),
              out.end());
    if (out.size() > maxCount) out.resize(maxCount);
    return out;
}

}  // namespace ics
