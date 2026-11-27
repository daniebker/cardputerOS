// Calendar: an agenda of subscribed calendars (.ics links, e.g. the "secret
// address in iCal format" of a Google calendar, a published Outlook or iCloud
// calendar, or any webcal:// link).
//
// Downloads follow the stale-while-revalidate pattern: the copy on the SD card is
// shown at once, and calendars whose copy is older than kFreshSec are checked in
// the background (conditional GET, so an unchanged calendar costs one small
// request). A newer copy replaces the old one only once it has been downloaded
// completely, and the agenda then updates in place. Without Wi-Fi, or if the
// server fails, the last copy stays in use and the header says how old it is.
#include <Arduino.h>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../hal/sdcard.h"
#include "../../os/icons.h"
#include "../../os/kernel.h"
#include "../../os/net.h"
#include "../../os/storage.h"
#include "../../os/theme.h"
#include "../../os/ui.h"
#include "calendar_fetch.h"
#include "calendar_store.h"
#include "ics.h"

namespace {

namespace dr = theme::dracula;

constexpr int64_t kFreshSec = 15 * 60;      // a copy checked this recently is used without asking the server
constexpr uint32_t kRetryMs = 10 * 60000;   // a failed background check is retried after this long
constexpr int kPastDays = 7, kFutureDays = 120;  // the agenda's window around today
constexpr size_t kMaxPerCalendar = 500;     // occurrences kept per calendar
constexpr int64_t kDay = 86400;
const char* kNoCard = "No microSD card. Calendars are kept on the card: insert one and press r.";

bool clockValid() { return time(nullptr) > 1700000000; }
int64_t now() { return (int64_t)time(nullptr); }
int localOffset() { return storage::getInt("tz_min", 0) * 60; }
// Local day number (days since 1970-01-01 in local time) of a UTC time.
int64_t localDay(int64_t t) {
    const int64_t x = t + localOffset();
    return x >= 0 ? x / kDay : (x - kDay + 1) / kDay;
}
int64_t dayStart(int64_t day) { return day * kDay - localOffset(); }

std::string hhmm(int64_t t) {
    const time_t tt = (time_t)t;
    struct tm tmv;
    localtime_r(&tt, &tmv);
    char buf[8];
    snprintf(buf, sizeof buf, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    return buf;
}

std::string dayName(int64_t day, bool withYear = false) {
    static const char* const kWd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* const kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int y, m, d;
    ics::civilFromDays(day, y, m, d);
    std::string s = std::string(kWd[ics::weekday(day)]) + " " + std::to_string(d) + " " + kMon[m - 1];
    if (withYear) s += " " + std::to_string(y);
    return s;
}

// "14:02", "yesterday 14:02", "3 days ago"
std::string ageText(int64_t t) {
    if (t <= 0) return "never";
    const int64_t days = localDay(now()) - localDay(t);
    if (days <= 0) return hhmm(t);
    if (days == 1) return "yesterday " + hhmm(t);
    return std::to_string((long long)days) + " days ago";
}

std::vector<std::string> wrapText(os::Gfx& g, const std::string& text, int maxW) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        const std::string para = text.substr(start, nl - start);
        std::string line;
        size_t i = 0;
        while (i <= para.size()) {
            const size_t sp = std::min(para.find(' ', i), para.size());
            std::string word = para.substr(i, sp - i);
            i = sp + 1;
            const std::string cand = line.empty() ? word : line + " " + word;
            if (g.textWidth(cand.c_str()) <= maxW) {
                line = cand;
                continue;
            }
            if (!line.empty()) lines.push_back(line);
            line = word;
            while (line.size() > 1 && g.textWidth(line.c_str()) > maxW) {
                size_t n = line.size() - 1;
                while (n > 1 && g.textWidth(line.substr(0, n).c_str()) > maxW) n--;
                lines.push_back(line.substr(0, n));
                line.erase(0, n);
            }
        }
        lines.push_back(line);
        start = nl + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

struct Entry {
    int64_t start = 0, end = 0;
    bool allDay = false;
    int cal = 0;       // calendar id
    uint32_t info = 0; // index into the calendar's infos
};

struct Info {
    std::string summary, location, description;
};

struct Parsed {
    std::vector<Entry> entries;
    std::vector<Info> infos;
};

struct Row {
    enum Kind { Day, Event, Empty } kind = Day;
    int64_t day = 0;
    int entry = -1;  // index into entries_
};

class CalendarApp : public os::App {
public:
    ~CalendarApp() override { stopJob(); }

    void onStart() override {
        subs_ = calstore::loadSubs();
        loadAll();
    }

    // Opening the app: show the cached copies (already loaded) and revalidate
    // the stale ones in the background.
    void onResume() override {
        paused_ = false;
        if (!cardOk_) loadAll();
        if (opening_) {
            opening_ = false;
            selectToday();
            queueStale(true);
        }
        redraw();
    }

    void onPause() override {
        opening_ = true;
        paused_ = true;
    }

    void onTick() override {
        service();
        // the day changed (or the clock was set): the window moves
        if (clockValid() && localDay(now()) != loadedDay_) {
            loadAll();
            selectToday();
            redraw();
        }
    }

    void onBackgroundTick() override {
        if (job_) pumpJob();  // finish a download that was started, but start no new one
    }

    bool keepAwake() override { return job_ != nullptr; }

    const char* statusText() override {
        status_ = headerStatus();
        return status_.c_str();
    }

    void onKey(const key::Event& ev) override {
        if (ev.opt() && ev.code == 'a') return manage();
        if (ev.opt() && ev.code == 's') return refreshNow();
        if (detail_ >= 0) return detailKey(ev);
        if (ev.is('r')) return refreshNow();
        if (ev.is('c')) return manage();
        if (ev.is('t')) {
            selectToday();
            return redraw();
        }
        if (ev.is(key::Left) || ev.is(key::Right) || ev.is('<') || ev.is('>') || ev.is(',') || ev.is('/')) {
            jumpDay(ev.is(key::Left) || ev.is('<') || ev.is(',') ? -1 : 1);
            return;
        }
        os::Gfx& g = os::gfx();
        const int before = list_.selected;
        const ui::List::Result res = list_.onKey(g, listRect_, ev);
        if (res == ui::List::Activated) {
            openDetail();
            return;
        }
        if (res == ui::List::Moved) skipDayRow(list_.selected < before ? -1 : 1);
    }

private:
    // ------------------------------------------------------------ data

    CalendarSub* findSub(int id) {
        for (auto& s : subs_)
            if (s.id == id) return &s;
        return nullptr;
    }

    // Reads every calendar's copy from the card (needs no network).
    void loadAll() {
        parsed_.clear();
        loadedDay_ = clockValid() ? localDay(now()) : -1;
        hal::SdSession session;
        cardOk_ = session.ok();
        if (cardOk_) {
            for (auto& s : subs_) {
                calstore::readMeta(s);
                if (s.cached) parse(s);
            }
        }
        rebuild();
    }

    // Parses one calendar's copy (inside an SdSession) into its entries.
    void parse(CalendarSub& s) {
        parsed_.erase(s.id);
        File f = hal::sdFs().open(calstore::icsPath(s.id).c_str(), "r");
        if (!f) return;
        if (f.size() > 64 * 1024) ui::busy("Reading " + s.title() + "...");
        const int64_t today = clockValid() ? localDay(now()) : 0;
        const int64_t from = dayStart(today - kPastDays), to = dayStart(today + kFutureDays + 1);
        ics::Calendar cal;
        cal.keepAfter = from;
        cal.maxText = 300;
        cal.maxEvents = 1500;
        char buf[1024];
        for (;;) {
            const int n = f.read((uint8_t*)buf, sizeof buf);
            if (n <= 0) break;
            cal.feed(buf, (size_t)n);
        }
        f.close();
        cal.finish();
        if (!cal.sawCalendar()) {
            s.error = "The saved copy is not a calendar; it will be downloaded again.";
            s.etag.clear();
            s.lastModified.clear();
            s.checkedAt = 0;
            return;
        }
        s.feedName = cal.name();
        Parsed p;
        if (clockValid()) {
            std::map<uint32_t, uint32_t> infoOf;  // event index -> info index
            for (const ics::Occurrence& o : cal.expand(from, to, localOffset(), kMaxPerCalendar)) {
                auto it = infoOf.find(o.event);
                if (it == infoOf.end()) {
                    const ics::Event& e = cal.events()[o.event];
                    p.infos.push_back({e.summary, e.location, e.description});
                    it = infoOf.emplace(o.event, (uint32_t)p.infos.size() - 1).first;
                }
                p.entries.push_back({o.start, o.end, o.allDay, s.id, it->second});
            }
        }
        parsed_[s.id] = std::move(p);
    }

    const Info& infoOf(const Entry& e) const { return parsed_.at(e.cal).infos[e.info]; }

    uint32_t colorOf(int cal) const {
        for (auto& s : subs_)
            if (s.id == cal) return calstore::kColors[s.color % calstore::kColorCount];
        return theme::text;
    }

    // Recomputes the merged agenda (keeping the selected event selected).
    void rebuild() {
        // Entries are re-indexed when a calendar is read again: remember the
        // selected and the opened event by what they are.
        struct Key {
            int64_t start = 0;
            int cal = -1;
            std::string summary;
        };
        auto keyOf = [this](int entry) {
            Key k;
            if (entry < 0 || entry >= (int)entries_.size() || !parsed_.count(entries_[entry].cal)) return k;
            const Entry& e = entries_[entry];
            const Parsed& p = parsed_.at(e.cal);
            if (e.info >= p.infos.size()) return k;
            k.start = e.start;
            k.cal = e.cal;
            k.summary = p.infos[e.info].summary;
            return k;
        };
        auto matches = [this](const Entry& e, const Key& k) {
            return k.cal >= 0 && e.cal == k.cal && e.start == k.start && infoOf(e).summary == k.summary;
        };
        Key keep, open;
        int64_t keepDay = INT64_MIN;
        if (list_.selected >= 0 && list_.selected < (int)rows_.size()) {
            keepDay = rows_[list_.selected].day;
            keep = keyOf(rows_[list_.selected].entry);
        }
        if (detail_ >= 0) open = keyOf(detail_);
        entries_.clear();
        for (auto& kv : parsed_)
            for (const Entry& e : kv.second.entries) entries_.push_back(e);
        std::stable_sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
            if (a.start != b.start) return a.start < b.start;
            return a.allDay && !b.allDay;
        });
        if (detail_ >= 0) {
            detail_ = -1;
            for (size_t i = 0; i < entries_.size() && detail_ < 0; i++)
                if (matches(entries_[i], open)) detail_ = (int)i;
        }

        rows_.clear();
        std::vector<ui::ListItem> items;
        if (clockValid() && cardOk_) {
            const int64_t today = localDay(now());
            const int64_t t = now();
            for (int64_t day = today - kPastDays; day <= today + kFutureDays; day++) {
                const int64_t ds = dayStart(day), de = ds + kDay;
                bool header = false;
                auto addHeader = [&]() {
                    if (header) return;
                    header = true;
                    rows_.push_back({Row::Day, day, -1});
                    std::string label = dayName(day, false);
                    if (day == today) label = "Today, " + label;
                    else if (day == today + 1) label = "Tomorrow, " + label;
                    else if (day == today - 1) label = "Yesterday, " + label;
                    int y, m, d, ty, tm, td;
                    ics::civilFromDays(day, y, m, d);
                    ics::civilFromDays(today, ty, tm, td);
                    if (y != ty) label += " " + std::to_string(y);
                    items.push_back({label, "", day == today ? dr::purple : day < today ? theme::dim : dr::comment});
                };
                for (size_t i = 0; i < entries_.size(); i++) {
                    const Entry& e = entries_[i];
                    if (e.start >= de) break;
                    const bool overlaps = e.end > ds || (e.end == e.start && e.start >= ds);
                    if (!overlaps) continue;
                    addHeader();
                    rows_.push_back({Row::Event, day, (int)i});
                    items.push_back(eventItem(e, ds, de, t));
                }
                if (day == today && !header) {
                    addHeader();
                    rows_.push_back({Row::Empty, day, -1});
                    items.push_back({"   No events", "", theme::dim});
                }
            }
        }
        int sel = -1;
        for (size_t k = 0; k < rows_.size() && sel < 0; k++) {
            const Row& r = rows_[k];
            if (r.entry >= 0 && r.day == keepDay && matches(entries_[r.entry], keep)) sel = (int)k;
        }
        for (size_t k = 0; k < rows_.size() && sel < 0; k++)
            if (rows_[k].day == keepDay && rows_[k].kind != Row::Day) sel = (int)k;
        list_.items.swap(items);
        list_.selected = sel >= 0 ? sel : list_.selected;
        list_.clampSelection();
        if (!cardOk_) list_.emptyText = "Insert a microSD card (r: retry)";
        else if (subs_.empty()) list_.emptyText = "No calendars yet: press c to add one";
        else if (!clockValid()) list_.emptyText = "Waiting for the clock (connect to Wi-Fi)";
        else list_.emptyText = "Nothing in the next months";
    }

    ui::ListItem eventItem(const Entry& e, int64_t ds, int64_t de, int64_t t) const {
        const Info& in = infoOf(e);
        std::string when;
        if (e.allDay || (e.start <= ds && e.end >= de)) when = "all day";
        else if (e.start < ds) when = "-" + hhmm(e.end);
        else if (e.end > e.start && e.end <= de) when = hhmm(e.start) + "-" + hhmm(e.end);
        else when = hhmm(e.start);
        ui::ListItem it;
        it.label = "   " + when + "  " + ui::displayable(in.summary.empty() ? "(no title)" : in.summary);
        std::string where = ui::displayable(in.location.substr(0, in.location.find('\n')));
        if (where.size() > 22) where = where.substr(0, 20) + "..";
        it.detail = where;
        const bool over = e.end > e.start ? e.end <= t : e.start < t;
        it.color = over ? theme::dim : colorOf(e.cal);
        return it;
    }

    // ------------------------------------------------------------ navigation

    void selectToday() {
        if (rows_.empty()) return;
        const int64_t today = clockValid() ? localDay(now()) : 0, t = now();
        int sel = -1, firstToday = -1;
        for (size_t k = 0; k < rows_.size(); k++) {
            const Row& r = rows_[k];
            if (r.day < today || r.kind == Row::Day) continue;
            if (r.day > today) {
                if (sel < 0) sel = firstToday >= 0 ? firstToday : (int)k;
                break;
            }
            if (firstToday < 0) firstToday = (int)k;
            // today: the first event that is not over yet
            if (r.entry >= 0 && entries_[r.entry].end > t) {
                sel = (int)k;
                break;
            }
        }
        if (sel < 0) sel = firstToday >= 0 ? firstToday : (int)rows_.size() - 1;
        list_.selected = sel;
        list_.clampSelection();
    }

    // Moving onto a day title moves on to its first event (or back to the previous day's last).
    void skipDayRow(int dir) {
        const int sel = list_.selected;
        if (sel < 0 || sel >= (int)rows_.size() || rows_[sel].kind != Row::Day) return;
        int next = sel + dir;
        if (next < 0 || next >= (int)rows_.size()) next = sel - dir;
        if (next < 0 || next >= (int)rows_.size()) return;
        list_.update(os::gfx(), listRect_, list_.items, next);
        os::refreshStatus();
    }

    void jumpDay(int dir) {
        if (rows_.empty()) return;
        const int sel = std::max(0, std::min(list_.selected, (int)rows_.size() - 1));
        const int64_t day = rows_[sel].day;
        int target = -1;
        if (dir > 0) {
            for (size_t k = sel; k < rows_.size(); k++)
                if (rows_[k].day > day && rows_[k].kind != Row::Day) {
                    target = (int)k;
                    break;
                }
        } else {
            int64_t prevDay = INT64_MIN;
            for (int k = sel; k >= 0; k--)
                if (rows_[k].day < day && rows_[k].kind != Row::Day) {
                    prevDay = rows_[k].day;
                    break;
                }
            for (size_t k = 0; k < rows_.size() && prevDay != INT64_MIN; k++)
                if (rows_[k].day == prevDay && rows_[k].kind != Row::Day) {
                    target = (int)k;
                    break;
                }
        }
        if (target < 0) return;
        list_.update(os::gfx(), listRect_, list_.items, target);
    }

    // ------------------------------------------------------------ detail screen

    void openDetail() {
        const int sel = list_.selected;
        if (sel < 0 || sel >= (int)rows_.size() || rows_[sel].entry < 0) return;
        detail_ = rows_[sel].entry;
        detailTop_ = 0;
        redraw();
    }

    void detailKey(const key::Event& ev) {
        if (ev.code == key::Esc || ev.is(key::Backspace) || ev.is(key::Left) || ev.is(key::Enter)) {
            detail_ = -1;
            return redraw();
        }
        int top = detailTop_;
        if (ui::isUp(ev)) top--;
        else if (ui::isDown(ev)) top++;
        else if (ev.is(key::PageUp)) top -= 8;
        else if (ev.is(key::PageDown) || ev.is(' ')) top += 8;
        top = std::max(0, std::min(top, detailMaxTop_));
        if (top != detailTop_) {
            detailTop_ = top;
            redraw();
        }
    }

    std::string whenText(const Entry& e) const {
        if (e.allDay) {
            const int64_t first = localDay(e.start), last = localDay(e.end - 1);
            if (last <= first) return dayName(first, true) + ", all day";
            return dayName(first, false) + " - " + dayName(last, true) + ", all day";
        }
        std::string s = dayName(localDay(e.start), true) + ", " + hhmm(e.start);
        if (e.end > e.start) {
            if (localDay(e.end - 1) == localDay(e.start)) s += " - " + hhmm(e.end);
            else s += " - " + dayName(localDay(e.end - 1), false) + " " + hhmm(e.end);
        }
        return s;
    }

    void drawDetail(os::Gfx& g, const os::Rect& c) {
        const Entry& e = entries_[detail_];
        const Info& in = infoOf(e);
        const CalendarSub* s = findSub(e.cal);
        os::Rect body = ui::header(g, c, s ? ui::displayable(s->title()).c_str() : "Event");
        body = ui::footer(g, body, "up / down: scroll   Esc / Backspace: back");
        g.setTextDatum(textdatum_t::top_left);
        const int w = body.w - 16;
        struct Line {
            std::string text;
            uint32_t color;
            const lgfx::IFont* font;
        };
        std::vector<Line> lines;
        auto add = [&](const std::string& text, uint32_t color, const lgfx::IFont* font) {
            g.setFont(font);
            for (auto& l : wrapText(g, ui::displayable(text), w)) lines.push_back({l, color, font});
        };
        add(in.summary.empty() ? "(no title)" : in.summary, colorOf(e.cal), theme::fontUi());
        add(whenText(e), theme::text, theme::fontUi());
        if (!in.location.empty()) add(in.location, dr::cyan, theme::fontUi());
        if (!in.description.empty()) {
            lines.push_back({"", theme::text, theme::fontSmall()});
            add(in.description, theme::text, theme::fontSmall());
        }
        g.setFont(theme::fontUi());
        const int lineH = g.fontHeight() + 2;
        const int rows = std::max(1, (body.h - 8) / lineH);
        detailMaxTop_ = std::max(0, (int)lines.size() - rows);
        detailTop_ = std::min(detailTop_, detailMaxTop_);
        int y = body.y + 4;
        for (int i = detailTop_; i < (int)lines.size() && y + lineH <= body.y + body.h; i++) {
            g.setFont(lines[i].font);
            g.setTextColor(lines[i].color);
            g.drawString(lines[i].text.c_str(), body.x + 8, y);
            y += g.fontHeight() + 2;
        }
    }

    // ------------------------------------------------------------ revalidation (background)

    // Queues the calendars whose copy is older than kFreshSec (or whose age is
    // unknown because the clock is not set yet).
    void queueStale(bool skipRecentFailures) {
        for (auto& s : subs_) {
            if (s.url.empty() || std::find(queue_.begin(), queue_.end(), s.id) != queue_.end()) continue;
            if (job_ && jobId_ == s.id) continue;
            const bool fresh = s.cached && s.error.empty() && clockValid() && s.checkedAt > 0 &&
                               now() - s.checkedAt < kFreshSec && now() >= s.checkedAt;
            if (fresh) continue;
            auto it = lastTry_.find(s.id);
            if (skipRecentFailures && it != lastTry_.end() && millis() - it->second < kRetryMs) continue;
            queue_.push_back(s.id);
        }
    }

    void service() {
        if (job_) {
            pumpJob();
            return;
        }
        if (queue_.empty() || !net::connected()) return;
        startJob(queue_.front());
        queue_.erase(queue_.begin());
    }

    void startJob(int id) {
        CalendarSub* s = findSub(id);
        if (!s) return;
        lastTry_[id] = millis();
        jobSession_.reset(new hal::SdSession());
        if (!jobSession_->ok() || !hal::sdMakeDirs(calstore::kDir)) {
            jobSession_.reset();
            cardOk_ = false;
            queue_.clear();
            rebuild();
            return redraw();
        }
        jobFile_ = hal::sdFs().open(calstore::tmpPath(id).c_str(), "w");
        if (!jobFile_) {
            jobSession_.reset();
            s->error = "Could not write to the SD card (full?).";
            return redraw();
        }
        jobHead_.clear();
        jobWriteOk_ = true;
        job_ = FetchJob::start(request(*s));
        if (!job_) {
            jobFile_.close();
            jobSession_.reset();
            s->error = "Not enough memory to download now.";
            return redraw();
        }
        jobId_ = id;
        os::refreshStatus();
        if (detail_ < 0 && isForeground()) drawHeader();
    }

    FetchRequest request(const CalendarSub& s) const {
        FetchRequest r;
        r.url = s.url;
        r.user = s.user;
        r.pass = s.pass;
        if (s.cached && s.error.empty()) {
            r.etag = s.etag;
            r.lastModified = s.lastModified;
        }
        return r;
    }

    bool writeChunk(File& f, std::string& head, bool& writeOk, const char* d, size_t n) {
        if (head.size() < 512) head.append(d, std::min(n, 512 - head.size()));
        if (writeOk && f.write((const uint8_t*)d, n) != n) writeOk = false;
        return writeOk;
    }

    void pumpJob() {
        const bool finished =
            job_->pump([this](const char* d, size_t n) { return writeChunk(jobFile_, jobHead_, jobWriteOk_, d, n); });
        if (!finished) return;
        jobFile_.close();
        const FetchResult res = job_->result();
        job_.reset();
        CalendarSub* s = findSub(jobId_);
        if (s) {
            applyResult(*s, res, jobHead_, jobWriteOk_);
            if (s->error.find("certificate") != std::string::npos) {
                s->error += " Press r to check the server's certificate.";
                calstore::writeMeta(*s);
            }
        }
        else calstore::removeFiles(jobId_);  // removed while downloading
        jobSession_.reset();
        jobHead_.clear();
        os::refreshStatus();
        if (isForeground()) redraw();
    }

    void stopJob() {
        if (!job_) return;
        job_->cancel();
        job_.reset();
        jobFile_.close();
        if (jobSession_) hal::sdFs().remove(calstore::tmpPath(jobId_).c_str());
        jobSession_.reset();
    }

    static bool looksLikeCalendar(const std::string& head) {
        std::string h;
        for (char c : head.substr(0, 400)) h.push_back((char)toupper((unsigned char)c));
        return h.find("BEGIN:VCALENDAR") != std::string::npos;
    }

    // Needs an open SdSession; the download (if any) is in the .tmp file.
    void applyResult(CalendarSub& s, const FetchResult& r, const std::string& head, bool writeOk) {
        const std::string tmp = calstore::tmpPath(s.id);
        bool reparse = false;
        if (r.ok && r.status == 304) {
            s.checkedAt = now();
            s.error.clear();
        } else if (r.ok && r.status == 200) {
            if (!writeOk) {
                s.error = "Could not save the calendar on the SD card (card full?).";
            } else if (!looksLikeCalendar(head)) {
                s.error = "The link did not return a calendar (.ics). Check that it is the iCal / ICS address.";
            } else if (!calstore::commitDownload(s.id)) {
                s.error = "Could not save the calendar on the SD card.";
            } else {
                s.etag = r.etag;
                s.lastModified = r.lastModified;
                s.checkedAt = s.fetchedAt = now();
                s.cached = true;
                s.error.clear();
                reparse = true;
            }
        } else {
            s.error = r.error.empty() ? "Download failed." : r.error;
        }
        if (hal::sdFs().exists(tmp.c_str())) hal::sdFs().remove(tmp.c_str());
        if (reparse) {
            parse(s);
            rebuild();
        }
        calstore::writeMeta(s);
        if (s.error.empty()) lastTry_.erase(s.id);
    }

    // ------------------------------------------------------------ refresh now (interactive)

    DavClient::CertFn certPrompt() {
        return [](const std::string& host, const std::string& fp, bool changed) {
            std::string shown;
            for (size_t i = 0; i < fp.size(); i += 4) shown += fp.substr(i, 4) + " ";
            if (changed)
                return ui::confirm("CERTIFICATE CHANGED",
                                   "The certificate of " + host + " is different from last time. That is normal after a "
                                   "renewal, but could also mean someone is intercepting the connection.\nSHA-256: " +
                                       shown + "\nTrust the new certificate?");
            return ui::confirm("New server",
                               "First connection to " + host + ".\nCertificate SHA-256:\n" + shown + "\nTrust this server?");
        };
    }

    // Checks the given calendars now (all if `only` is 0), asking about certificates.
    void refreshNow(int only = 0) {
        detail_ = -1;
        if (subs_.empty()) {
            ui::alert("Calendar", "No calendars yet. Press c to add one.");
            return redraw();
        }
        if (!cardOk_) loadAll();
        if (!cardOk_) {
            ui::alert("Calendar", kNoCard);
            return redraw();
        }
        if (!net::connected()) {
            ui::busy("Connecting to Wi-Fi...");
            if (!net::connectSaved(20000, os::idle)) {
                ui::alert("Calendar", "Wi-Fi is not connected. The saved copies stay in use.");
                return redraw();
            }
        }
        stopJob();  // a background check of the same calendars would be redundant
        queue_.clear();
        {
            const uint32_t t0 = millis();  // the clock is set from the network right after connecting
            while (!clockValid() && millis() - t0 < 3000) os::idle();
        }
        hal::SdSession session;
        if (!session.ok() || !hal::sdMakeDirs(calstore::kDir)) {
            cardOk_ = false;
            ui::alert("Calendar", kNoCard);
            rebuild();
            return redraw();
        }
        std::string failures;
        for (auto& s : subs_) {
            if (only && s.id != only) continue;
            if (s.url.empty()) continue;
            ui::busy("Updating " + ui::displayable(s.title()) + "...");
            File f = hal::sdFs().open(calstore::tmpPath(s.id).c_str(), "w");
            std::string head;
            bool writeOk = (bool)f;
            FetchResult r;
            if (!f) {
                r.error = "Could not write to the SD card (full?).";
            } else {
                r = fetchCalendar(request(s), certPrompt(),
                                  [&](const char* d, size_t n) { return writeChunk(f, head, writeOk, d, n); });
                f.close();
            }
            applyResult(s, r, head, writeOk);
            if (!s.error.empty()) failures += ui::displayable(s.title()) + ": " + s.error + "\n";
        }
        rebuild();
        os::refreshStatus();
        if (!failures.empty()) ui::alert("Some calendars were not updated", failures + "The saved copies stay in use.");
        redraw();
    }

    // ------------------------------------------------------------ managing calendars

    void manage() {
        detail_ = -1;
        int initial = 0;
        for (;;) {
            std::vector<std::string> items;
            for (auto& s : subs_) {
                std::string line = ui::displayable(s.title());
                if (!s.error.empty()) line += "  (error)";
                else if (!s.cached) line += "  (not downloaded)";
                else line += "  (" + ageText(s.checkedAt) + ")";
                items.push_back(line);
            }
            if ((int)subs_.size() < calstore::kMaxCalendars) items.push_back("+ Add calendar");
            const int pick = ui::choose("Calendars", items, initial);
            if (pick < 0) break;
            initial = pick;
            if (pick >= (int)subs_.size()) {
                addCalendar();
                break;
            }
            if (!editCalendar(subs_[pick].id)) break;
        }
        rebuild();
        redraw();
    }

    // Asks for the link (and a login if needed). False if cancelled.
    bool askLink(CalendarSub& s) {
        for (;;) {
            if (!ui::prompt("Calendar link (.ics)", s.url, false,
                            "https:// or webcal:// address of the calendar in iCal format"))
                return false;
            dav::Url u;
            if (dav::parseUrl(calendarHttpUrl(s.url), u)) break;
            ui::alert("Calendar", "The link must start with https://, http:// or webcal://");
        }
        if (ui::confirm("Login", "Does this link need a user name and password? Most calendar links do not.",
                        !s.user.empty())) {
            if (!ui::prompt("User name", s.user) || !ui::prompt("Password", s.pass, true, "Stored encrypted on this device."))
                return false;
        } else {
            s.user.clear();
            s.pass.clear();
        }
        return true;
    }

    void addCalendar() {
        CalendarSub s;
        s.id = calstore::newId(subs_);
        s.color = (int)subs_.size() % calstore::kColorCount;
        if (!askLink(s)) return;
        if (!ui::prompt("Name", s.name, false, "Empty: use the calendar's own name")) return;
        subs_.push_back(s);
        calstore::saveSubs(subs_);
        refreshNow(s.id);
    }

    // False when the calendar was removed.
    bool editCalendar(int id) {
        CalendarSub* s = findSub(id);
        if (!s) return false;
        const std::vector<std::string> actions = {"Update now", "Rename", "Change link", "Colour: " +
                                                  std::string(calstore::kColorNames[s->color % calstore::kColorCount]),
                                                  "Info", "Remove"};
        const int pick = ui::choose(ui::displayable(s->title()).c_str(), actions, 0);
        switch (pick) {
            case 0: refreshNow(id); break;
            case 1:
                if (ui::prompt("Name", s->name, false, "Empty: use the calendar's own name")) calstore::saveSubs(subs_);
                break;
            case 2: {
                CalendarSub edited = *s;
                if (askLink(edited)) {
                    const bool changed = edited.url != s->url;
                    *s = edited;
                    if (changed) {
                        s->etag.clear();
                        s->lastModified.clear();
                        s->checkedAt = 0;
                    }
                    calstore::saveSubs(subs_);
                    refreshNow(id);
                }
                break;
            }
            case 3:
                s->color = (s->color + 1) % calstore::kColorCount;
                calstore::saveSubs(subs_);
                rebuild();
                break;
            case 4: {
                std::string text = "Last checked: " + ageText(s->checkedAt) + "\nLast changed: " + ageText(s->fetchedAt);
                auto it = parsed_.find(id);
                if (it != parsed_.end()) text += "\nEvents shown: " + std::to_string(it->second.entries.size());
                if (!s->error.empty()) text += "\nLast error: " + s->error;
                ui::alert(ui::displayable(s->title()).c_str(), text);
                break;
            }
            case 5:
                if (ui::confirm("Remove calendar", "Remove \"" + ui::displayable(s->title()) +
                                                       "\"? Its copy on the SD card is deleted.")) {
                    if (job_ && jobId_ == id) stopJob();
                    queue_.erase(std::remove(queue_.begin(), queue_.end(), id), queue_.end());
                    {
                        hal::SdSession session;
                        if (session.ok()) calstore::removeFiles(id);
                    }
                    calstore::forgetSecrets(id);
                    parsed_.erase(id);
                    subs_.erase(std::remove_if(subs_.begin(), subs_.end(), [id](const CalendarSub& c) { return c.id == id; }),
                                subs_.end());
                    calstore::saveSubs(subs_);
                    rebuild();
                    return false;
                }
                break;
            default: return false;
        }
        return true;
    }

    // ------------------------------------------------------------ drawing

    bool isForeground() const { return !paused_; }

    std::string headerStatus() const {
        if (job_) {
            for (auto& s : subs_)
                if (s.id == jobId_) return "updating " + ui::displayable(s.title()) + "...";
        }
        if (!cardOk_) return "no SD card";
        if (subs_.empty()) return "no calendars";
        int failed = 0;
        int64_t oldest = INT64_MAX;
        for (auto& s : subs_) {
            if (!s.error.empty()) failed++;
            if (s.cached) oldest = std::min(oldest, s.checkedAt);
        }
        std::string st;
        if (oldest != INT64_MAX) st = "as of " + ageText(oldest);
        if (!net::connected() && oldest != INT64_MAX && (!clockValid() || now() - oldest >= kFreshSec))
            st = "offline, " + st;
        if (failed) st += (st.empty() ? "" : ", ") + std::to_string(failed) + " failed";
        return st;
    }

    void drawHeader() {
        os::Gfx& g = os::gfx();
        const os::Rect c = os::content();
        const std::string title = "Calendar   - " + headerStatus();
        ui::header(g, c, title.c_str());
    }

    void redraw() {
        os::Gfx& g = os::gfx();
        const os::Rect c = os::content();
        g.fillRect(c.x, c.y, c.w, c.h, theme::bg);
        if (detail_ >= 0 && detail_ < (int)entries_.size()) return drawDetail(g, c);
        detail_ = -1;
        const std::string status = headerStatus();
        const std::string title = status.empty() ? std::string("Calendar") : "Calendar   - " + status;
        os::Rect body = ui::header(g, c, title.c_str());
        body = ui::footer(g, body, "Enter: details  t: today  </>: day  r: update  c: calendars");
        listRect_ = body;
        list_.draw(g, listRect_);
    }

    std::vector<CalendarSub> subs_;
    std::map<int, Parsed> parsed_;
    std::vector<Entry> entries_;
    std::vector<Row> rows_;
    ui::List list_;
    os::Rect listRect_{0, 0, 0, 0};
    std::string status_;
    int detail_ = -1, detailTop_ = 0, detailMaxTop_ = 0;
    int64_t loadedDay_ = -1;
    bool cardOk_ = false, opening_ = true, paused_ = false;

    // background revalidation
    std::vector<int> queue_;
    std::map<int, uint32_t> lastTry_;  // millis() of the last background attempt per calendar
    std::shared_ptr<FetchJob> job_;
    int jobId_ = 0;
    std::unique_ptr<hal::SdSession> jobSession_;
    File jobFile_;
    std::string jobHead_;
    bool jobWriteOk_ = true;
};

OS_REGISTER_APP(CalendarApp, "Calendar", "Agenda of .ics calendar links", theme::dracula::cyan, icons::calendar, 23);

}  // namespace
