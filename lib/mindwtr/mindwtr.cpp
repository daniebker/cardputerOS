#include "mindwtr.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mindwtr {

const char* const kStatuses[] = {"inbox", "next", "waiting", "someday", "reference", "done", "archived"};

bool validStatus(const std::string& s) {
    for (int i = 0; i < kStatusCount; i++)
        if (s == kStatuses[i]) return true;
    return false;
}

bool finished(const std::string& status) { return status == "done" || status == "archived"; }

bool summarize(const json::Value& obj, Task& out) {
    if (obj.type != json::Value::Object) return false;
    out = Task();
    out.id = obj.getString("id");
    if (out.id.empty()) return false;
    out.title = obj.getString("title");
    out.status = obj.getString("status");
    if (!validStatus(out.status)) out.status = "inbox";
    out.dueDate = obj.getString("dueDate");
    out.startTime = obj.getString("startTime");
    out.projectId = obj.getString("projectId");
    out.completedAt = obj.getString("completedAt");
    out.description = obj.getString("description");
    out.contexts = obj.getStrings("contexts");
    out.tags = obj.getStrings("tags");
    out.deleted = !obj.getString("deletedAt").empty() || !obj.getString("purgedAt").empty();
    if (const json::Value* r = obj.get("recurrence")) {
        out.recurring = r->type == json::Value::Object || (r->type == json::Value::String && !r->str.empty());
    }
    if (const json::Value* c = obj.get("checklist")) {
        if (c->type == json::Value::Array) {
            for (auto& item : c->arr) {
                if (item.type != json::Value::Object) continue;
                out.checklistTotal++;
                const json::Value* done = item.get("isCompleted");
                if (done && done->type == json::Value::Bool && done->b) out.checklistDone++;
            }
        }
    }
    return true;
}

bool summarize(const json::Value& obj, TaskRow& out, size_t maxTitle) {
    if (obj.type != json::Value::Object) return false;
    out = TaskRow();
    out.id = obj.getString("id");
    if (out.id.empty()) return false;
    out.title = obj.getString("title");
    if (out.title.size() > maxTitle) {
        size_t n = maxTitle;
        while (n > 0 && ((unsigned char)out.title[n] & 0xC0) == 0x80) n--;  // not inside a UTF-8 character
        out.title.resize(n);
    }
    out.status = obj.getString("status");
    if (!validStatus(out.status)) out.status = "inbox";
    out.dueDate = obj.getString("dueDate");
    out.completedKey = minuteKey(obj.getString("completedAt"));
    out.deleted = !obj.getString("deletedAt").empty() || !obj.getString("purgedAt").empty();
    if (const json::Value* r = obj.get("recurrence")) {
        out.recurring = r->type == json::Value::Object || (r->type == json::Value::String && !r->str.empty());
    }
    return true;
}

bool summarize(const json::Value& obj, Project& out) {
    if (obj.type != json::Value::Object) return false;
    out = Project();
    out.id = obj.getString("id");
    if (out.id.empty()) return false;
    out.title = obj.getString("title");
    out.status = obj.getString("status");
    out.deleted = !obj.getString("deletedAt").empty() || !obj.getString("purgedAt").empty();
    return true;
}

// ---------------------------------------------------------------- patches

void Patch::setField(const std::string& key, json::Value v) {
    unset.erase(std::remove(unset.begin(), unset.end(), key), unset.end());
    set.set(key, std::move(v));
}

void Patch::unsetField(const std::string& key) {
    set.erase(key);
    if (std::find(unset.begin(), unset.end(), key) == unset.end()) unset.push_back(key);
}

void applyFields(json::Value& task, const Patch& p) {
    if (task.type != json::Value::Object) task = json::Value::object();
    for (auto& k : p.unset) task.erase(k);
    for (auto& m : p.set.obj) task.set(m.first, m.second);
}

void applyForUpload(json::Value& task, const Patch& p, const std::string& deviceId) {
    const long long rev = p.isNew ? 0 : task.getInt("rev", 0);
    applyFields(task, p);
    task.set("rev", json::Value::number(rev < 0 ? 1 : rev + 1));
    if (!deviceId.empty()) task.set("revBy", json::Value::string(deviceId));
}

std::string serializePatches(const std::vector<Patch>& patches) {
    json::Value arr = json::Value::array();
    for (auto& p : patches) {
        json::Value o = json::Value::object();
        o.set("id", json::Value::string(p.id));
        if (p.isNew) o.set("new", json::Value::boolean(true));
        o.set("set", p.set);
        json::Value un = json::Value::array();
        for (auto& k : p.unset) un.arr.push_back(json::Value::string(k));
        o.set("unset", un);
        arr.arr.push_back(std::move(o));
    }
    return json::serialize(arr);
}

bool parsePatches(const std::string& text, std::vector<Patch>& out) {
    out.clear();
    json::Value arr;
    if (!json::parse(text, arr) || arr.type != json::Value::Array) return false;
    for (auto& o : arr.arr) {
        Patch p;
        p.id = o.getString("id");
        if (p.id.empty()) continue;
        const json::Value* n = o.get("new");
        p.isNew = n && n->type == json::Value::Bool && n->b;
        if (const json::Value* s = o.get("set"))
            if (s->type == json::Value::Object) p.set = *s;
        p.unset = o.getStrings("unset");
        out.push_back(std::move(p));
    }
    return true;
}

void changeStatus(Patch& p, const json::Value& task, const std::string& status, const std::string& nowIso) {
    const std::string old = task.getString("status");
    p.setField("status", json::Value::string(status));
    if (status == old) return;
    const std::string completedAt = task.getString("completedAt");
    if (status == "done") {
        p.setField("completedAt", json::Value::string(old == "archived" && !completedAt.empty() ? completedAt : nowIso));
        p.unsetField("cancelledAt");
        p.setField("isFocusedToday", json::Value::boolean(false));
        p.unsetField("focusOrder");
    } else if (status == "archived") {
        p.setField("archivedAt", json::Value::string(nowIso));
        p.setField("completedAt", json::Value::string(completedAt.empty() ? nowIso : completedAt));
        p.setField("isFocusedToday", json::Value::boolean(false));
        p.unsetField("focusOrder");
    } else if (finished(old)) {
        p.unsetField("completedAt");
        p.unsetField("cancelledAt");
    }
    if (old == "archived" && status != "archived") p.unsetField("archivedAt");
}

Patch newTask(const std::string& id, const std::string& title, const std::string& status, const std::string& nowIso) {
    Patch p;
    p.id = id;
    p.isNew = true;
    p.set.set("id", json::Value::string(id));
    p.set.set("title", json::Value::string(title));
    p.set.set("status", json::Value::string(validStatus(status) ? status : "inbox"));
    p.set.set("tags", json::Value::array());
    p.set.set("contexts", json::Value::array());
    p.set.set("createdAt", json::Value::string(nowIso));
    p.set.set("updatedAt", json::Value::string(nowIso));
    if (status == "done") p.set.set("completedAt", json::Value::string(nowIso));
    return p;
}

// ---------------------------------------------------------------- dates

long daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

Date civilFromDays(long z) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    Date r;
    r.d = (int)(doy - (153 * mp + 2) / 5 + 1);
    r.m = (int)(mp < 10 ? mp + 3 : mp - 9);
    r.y = (int)((long)yoe + era * 400 + (r.m <= 2));
    return r;
}

int weekday(const Date& d) {
    const long z = daysFromCivil(d.y, d.m, d.d);
    return (int)(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

namespace {

bool validCivil(int y, int m, int d) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (y < 1900 || y > 9999 || m < 1 || m > 12 || d < 1) return false;
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return d <= (m == 2 && leap ? 29 : kDays[m - 1]);
}

bool digitsAt(const std::string& s, size_t pos, size_t n) {
    if (s.size() < pos + n) return false;
    for (size_t i = pos; i < pos + n; i++)
        if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

std::string fmtDay(const Date& d) {
    char buf[16];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d", d.y, d.m, d.d);
    return buf;
}

Date addDays(const Date& d, long n) { return civilFromDays(daysFromCivil(d.y, d.m, d.d) + n); }

Date addMonths(const Date& d, int n) {
    int m0 = d.y * 12 + (d.m - 1) + n;
    Date r;
    r.y = m0 / 12;
    r.m = m0 % 12 + 1;
    r.d = d.d;
    while (r.d > 28 && !validCivil(r.y, r.m, r.d)) r.d--;
    return r;
}

// "14:30", "9:05", "14", "2pm", "2:30pm" -> minutes since midnight
bool parseTime(const std::string& t, int& minutes) {
    std::string s = t;
    int add = -1;  // 0: am, 12: pm
    if (s.size() > 2 && (s.compare(s.size() - 2, 2, "am") == 0 || s.compare(s.size() - 2, 2, "pm") == 0)) {
        add = s[s.size() - 2] == 'p' ? 12 : 0;
        s.resize(s.size() - 2);
    }
    if (s.empty() || s.size() > 5) return false;
    int h = 0, m = 0;
    const size_t colon = s.find(':');
    const std::string hs = colon == std::string::npos ? s : s.substr(0, colon);
    if (hs.empty() || hs.size() > 2 || !digitsAt(hs, 0, hs.size())) return false;
    h = atoi(hs.c_str());
    if (colon != std::string::npos) {
        const std::string ms = s.substr(colon + 1);
        if (ms.size() != 2 || !digitsAt(ms, 0, 2)) return false;
        m = atoi(ms.c_str());
    }
    if (add >= 0) {
        if (h < 1 || h > 12) return false;
        h = h % 12 + add;
    }
    if (h > 23 || m > 59) return false;
    minutes = h * 60 + m;
    return true;
}

bool parseDatePhrase(const std::string& s, const Date& today, Date& out) {
    if (s == "today" || s == "tod" || s == "t" || s == "now") {
        out = today;
        return true;
    }
    if (s == "tomorrow" || s == "tom" || s == "tmr") {
        out = addDays(today, 1);
        return true;
    }
    if (s == "yesterday") {
        out = addDays(today, -1);
        return true;
    }
    if (s == "next week" || s == "nw") {
        out = addDays(today, 7);
        return true;
    }
    if (s == "next month" || s == "nm") {
        out = addMonths(today, 1);
        return true;
    }
    if (s.size() >= 2 && s[0] == '+') {
        size_t i = 1;
        while (i < s.size() && isdigit((unsigned char)s[i])) i++;
        if (i == 1 || i > 5) return false;
        const int n = atoi(s.substr(1, i - 1).c_str());
        const std::string unit = s.substr(i);
        if (unit.empty() || unit == "d") out = addDays(today, n);
        else if (unit == "w") out = addDays(today, 7L * n);
        else if (unit == "m") out = addMonths(today, n);
        else if (unit == "y") out = addMonths(today, 12 * n);
        else return false;
        return true;
    }
    static const char* const kDays[] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};
    if (s.size() >= 2 && isalpha((unsigned char)s[0])) {
        for (int wd = 0; wd < 7; wd++) {
            if (s.size() <= strlen(kDays[wd]) && strncmp(kDays[wd], s.c_str(), s.size()) == 0) {
                int diff = (wd - weekday(today) + 7) % 7;
                if (diff == 0) diff = 7;
                out = addDays(today, diff);
                return true;
            }
        }
        return false;
    }
    // 2026-11-30
    if (s.size() == 10 && digitsAt(s, 0, 4) && s[4] == '-' && digitsAt(s, 5, 2) && s[7] == '-' && digitsAt(s, 8, 2)) {
        out = {atoi(s.substr(0, 4).c_str()), atoi(s.substr(5, 2).c_str()), atoi(s.substr(8, 2).c_str())};
        return validCivil(out.y, out.m, out.d);
    }
    // 11-30 (month-day) or 30.11 / 30.11.2026 (day.month)
    int y = 0, m = 0, d = 0;
    const size_t dash = s.find('-'), dot = s.find('.');
    if (dash != std::string::npos && dot == std::string::npos) {
        const std::string a = s.substr(0, dash), b = s.substr(dash + 1);
        if (a.empty() || b.empty() || a.size() > 2 || b.size() > 2 || !digitsAt(a, 0, a.size()) || !digitsAt(b, 0, b.size()))
            return false;
        m = atoi(a.c_str());
        d = atoi(b.c_str());
    } else if (dot != std::string::npos && dash == std::string::npos) {
        const std::string a = s.substr(0, dot);
        std::string b = s.substr(dot + 1), c;
        const size_t dot2 = b.find('.');
        if (dot2 != std::string::npos) {
            c = b.substr(dot2 + 1);
            b.resize(dot2);
        }
        if (a.empty() || b.empty() || a.size() > 2 || b.size() > 2 || !digitsAt(a, 0, a.size()) || !digitsAt(b, 0, b.size()))
            return false;
        d = atoi(a.c_str());
        m = atoi(b.c_str());
        if (!c.empty()) {
            if (c.size() != 4 || !digitsAt(c, 0, 4)) return false;
            y = atoi(c.c_str());
        }
    } else {
        return false;
    }
    if (y == 0) {
        y = today.y;
        if (validCivil(y, m, d) && daysFromCivil(y, m, d) < daysFromCivil(today.y, today.m, today.d)) y++;
    }
    out = {y, m, d};
    return validCivil(y, m, d);
}

std::string lower(const std::string& s) {
    std::string out;
    for (char c : s) out.push_back((char)tolower((unsigned char)c));
    return out;
}

}  // namespace

bool parseDay(const std::string& s, Date& out) {
    if (!(digitsAt(s, 0, 4) && s.size() >= 10 && s[4] == '-' && digitsAt(s, 5, 2) && s[7] == '-' && digitsAt(s, 8, 2)))
        return false;
    if (s.size() > 10 && s[10] != 'T' && s[10] != ' ') return false;
    out = {atoi(s.substr(0, 4).c_str()), atoi(s.substr(5, 2).c_str()), atoi(s.substr(8, 2).c_str())};
    return validCivil(out.y, out.m, out.d);
}

std::string timeOf(const std::string& s) {
    if (s.size() >= 16 && (s[10] == 'T' || s[10] == ' ') && digitsAt(s, 11, 2) && s[13] == ':' && digitsAt(s, 14, 2))
        return s.substr(11, 5);
    return std::string();
}

bool parseDateInput(const std::string& input, const Date& today, std::string& out) {
    std::string s = lower(input);
    for (char& c : s)
        if (c == '\t') c = ' ';
    // trim and collapse spaces
    std::vector<std::string> words;
    size_t pos = 0;
    while (pos < s.size()) {
        while (pos < s.size() && s[pos] == ' ') pos++;
        size_t e = s.find(' ', pos);
        if (e == std::string::npos) e = s.size();
        if (e > pos) words.push_back(s.substr(pos, e - pos));
        pos = e;
    }
    if (words.empty() || (words.size() == 1 && (words[0] == "-" || words[0] == "none" || words[0] == "clear"))) {
        out.clear();
        return true;
    }
    // ISO "2026-11-30T14:30"
    if (words.size() == 1 && words[0].size() > 11 && words[0][10] == 't') {
        const std::string w = words[0];
        words = {w.substr(0, 10), w.substr(11)};
    }
    int minutes = -1;
    if (words.size() >= 2 && parseTime(words.back(), minutes)) {
        words.pop_back();
        if (!words.empty() && words.back() == "at") words.pop_back();
    } else if (words.size() == 1 && words[0].find(':') != std::string::npos && parseTime(words[0], minutes)) {
        words.clear();  // just a time: today
    }
    std::string phrase;
    for (auto& w : words) phrase += (phrase.empty() ? "" : " ") + w;
    Date d = today;
    if (!phrase.empty() && !parseDatePhrase(phrase, today, d)) return false;
    out = fmtDay(d);
    if (minutes >= 0) {
        char buf[16];
        snprintf(buf, sizeof buf, "T%02d:%02d", minutes / 60, minutes % 60);
        out += buf;
    }
    return true;
}

int64_t minuteKey(const std::string& timestamp) {
    Date d;
    if (!parseDay(timestamp, d)) return 0;
    int64_t key = (int64_t)daysFromCivil(d.y, d.m, d.d) * 1440;
    const std::string t = timeOf(timestamp);
    if (!t.empty()) key += atoi(t.substr(0, 2).c_str()) * 60 + atoi(t.substr(3, 2).c_str());
    return key;
}

bool daysUntil(const std::string& date, const Date& today, long& out) {
    Date d;
    if (!parseDay(date, d)) return false;
    out = daysFromCivil(d.y, d.m, d.d) - daysFromCivil(today.y, today.m, today.d);
    return true;
}

std::string dateLabel(const std::string& date, const Date& today) {
    Date d;
    if (!parseDay(date, d)) return date;
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const char* const kWd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const long diff = daysFromCivil(d.y, d.m, d.d) - daysFromCivil(today.y, today.m, today.d);
    std::string out;
    char buf[32];
    if (diff == 0) out = "today";
    else if (diff == 1) out = "tomorrow";
    else if (diff == -1) out = "yesterday";
    else if (d.y == today.y || (diff > 0 && diff < 180)) {
        snprintf(buf, sizeof buf, "%s %d %s", kWd[weekday(d)], d.d, kMonths[d.m - 1]);
        out = buf;
    } else {
        snprintf(buf, sizeof buf, "%d %s %d", d.d, kMonths[d.m - 1], d.y);
        out = buf;
    }
    const std::string t = timeOf(date);
    if (!t.empty()) out += " " + t;
    return out;
}

std::string isoTimestamp(int64_t t) {
    int64_t days = t / 86400, rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    const Date d = civilFromDays((long)days);
    char buf[32];
    snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.000Z", d.y, d.m, d.d, (int)(rem / 3600), (int)(rem / 60 % 60),
             (int)(rem % 60));
    return buf;
}

std::string uuidFromBytes(const uint8_t in[16]) {
    uint8_t b[16];
    memcpy(b, in, 16);
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(hex[b[i] >> 4]);
        out.push_back(hex[b[i] & 15]);
    }
    return out;
}

}  // namespace mindwtr
