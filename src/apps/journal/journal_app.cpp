// Journal: quick timestamped entries, one Markdown file per day, synced with a
// WebDAV folder.
#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

#include "../../os/icons.h"
#include "../../os/kernel.h"
#include "../../os/net.h"
#include "../../os/storage.h"
#include "../../os/theme.h"
#include "../../os/ui.h"
#include "journal.h"
#include "journal_store.h"
#include "journal_sync.h"

namespace {

namespace dr = theme::dracula;
constexpr int kCw = 8, kLineH = 16;  // 8x16 fixed-width font
constexpr int kMaxEntryBytes = 400;
constexpr int kInputH = 20;

bool clockValid() { return time(nullptr) > 1700000000; }

std::string today() {
    const time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[16];
    strftime(buf, sizeof buf, "%Y-%m-%d", &tmv);
    return buf;
}

// "2026-11-12" -> "Thu 12 Nov 2026"
std::string prettyDate(const std::string& date) {
    if (!journal::validDate(date)) return date;
    struct tm tmv = {};
    tmv.tm_year = atoi(date.substr(0, 4).c_str()) - 1900;
    tmv.tm_mon = atoi(date.substr(5, 2).c_str()) - 1;
    tmv.tm_mday = atoi(date.substr(8, 2).c_str());
    tmv.tm_hour = 12;
    mktime(&tmv);  // fills in the weekday
    char buf[24];
    strftime(buf, sizeof buf, "%a %d %b %Y", &tmv);
    return buf;
}

class JournalApp : public os::App {
public:
    void onStart() override {
        store_.load();
        account_ = JournalAccount::load();
    }

    void onResume() override {
        if (viewDate_.empty()) viewDate_ = clockValid() ? today() : (store_.days().empty() ? "" : store_.days().back().date);
        redraw();
        if (!autoSynced_ && account_.configured() && net::connected()) {
            autoSynced_ = true;
            sync(false, true);
        }
    }

    // Leaving the app: push new entries to the server if we can, without asking anything.
    void onPause() override {
        if (store_.unsyncedCount() && account_.configured() && net::connected()) sync(false, false);
    }

    const char* statusText() override {
        status_ = prettyDate(viewDate_);
        const int n = store_.unsyncedCount();
        if (n) status_ += "  (" + std::to_string(n) + " not synced)";
        return status_.c_str();
    }

    void onKey(const key::Event& ev) override {
        if (ev.opt()) {
            if (ev.code == 's') sync(true, true);
            else if (ev.code == 'a') setupAccount();
            else if (ev.code == 'd') chooseDay();
            else if (ev.code == 'u') removeLast();
            return;
        }
        if (ev.code == key::Left || ev.code == key::Right) {
            stepDay(ev.code == key::Left ? -1 : 1);
        } else if (ev.code == key::Up || ev.code == key::PageUp) {
            scroll_ += ev.code == key::Up ? 1 : 6;
            redraw();
        } else if (ev.code == key::Down || ev.code == key::PageDown) {
            scroll_ -= ev.code == key::Down ? 1 : 6;
            redraw();
        } else if (ev.is(key::Enter)) {
            addEntry();
        } else if (ev.code == key::Backspace) {
            if (ev.ctrl()) input_.clear();
            else if (!input_.empty()) input_.pop_back();
            hint_.clear();
            drawInput();
        } else if (ev.isChar() && (int)input_.size() < kMaxEntryBytes) {
            input_.push_back((char)ev.code);
            hint_.clear();
            drawInput();
        }
    }

private:
    // ------------------------------------------------------------ actions
    void addEntry() {
        if (input_.find_first_not_of(' ') == std::string::npos) return;
        if (!clockValid()) {
            hint_ = "Clock not set yet (needs Wi-Fi once)";
            const std::string keep = input_;
            input_.clear();
            drawInput();
            input_ = keep;
            return;
        }
        const time_t t = time(nullptr);
        struct tm tmv;
        localtime_r(&t, &tmv);
        const std::string date = today();
        const std::string text = journal::addEntry(store_.text(date), date, tmv.tm_hour * 60 + tmv.tm_min, input_);
        if (!store_.setText(date, text, true) || !store_.save()) {
            hint_ = "Could not save (storage full?)";
        } else {
            input_.clear();
            hint_.clear();
        }
        viewDate_ = date;  // entries always go to today
        scroll_ = 0;
        os::refreshStatus();
        redraw();
    }

    void removeLast() {
        const std::string date = viewDate_;
        std::string text = store_.text(date);
        const auto es = journal::entries(text);
        if (es.empty()) return;
        std::string preview = ui::displayable(es.back().text);
        if (preview.size() > 60) preview = preview.substr(0, 58) + "..";
        if (ui::confirm("Remove entry", "Remove the last entry of " + prettyDate(date) + "?\n\"" + preview + "\"")) {
            journal::removeLastEntry(text);
            store_.setText(date, text, true);
            store_.save();
            os::refreshStatus();
        }
        redraw();
    }

    // All days that can be shown: stored days plus today.
    std::vector<std::string> allDays() const {
        std::vector<std::string> out;
        for (auto& d : store_.days()) out.push_back(d.date);
        if (clockValid() && std::find(out.begin(), out.end(), today()) == out.end()) out.push_back(today());
        std::sort(out.begin(), out.end());
        return out;
    }

    void stepDay(int dir) {
        const auto days = allDays();
        if (days.empty()) return;
        int i = (int)(std::lower_bound(days.begin(), days.end(), viewDate_) - days.begin());
        if (dir < 0) i--;
        else if (i < (int)days.size() && days[i] == viewDate_) i++;
        if (i < 0 || i >= (int)days.size()) return;
        viewDate_ = days[i];
        scroll_ = 0;
        os::refreshStatus();
        redraw();
    }

    void chooseDay() {
        auto days = allDays();
        if (days.empty()) return;
        std::reverse(days.begin(), days.end());  // newest first
        std::vector<std::string> names;
        int initial = 0;
        for (size_t i = 0; i < days.size(); i++) {
            names.push_back(prettyDate(days[i]) + "   (" + std::to_string(journal::entries(store_.text(days[i])).size()) + ")");
            if (days[i] == viewDate_) initial = (int)i;
        }
        const int pick = ui::choose("Journal days", names, initial);
        if (pick >= 0) {
            viewDate_ = days[pick];
            scroll_ = 0;
            os::refreshStatus();
        }
        redraw();
    }

    // ------------------------------------------------------------ sync
    void setupAccount() {
        JournalAccount a = account_;
        if (!a.configured() && storage::getStr("dav_kind") == "webdav") {
            // start from the Notes account: same server, a "Journal" folder next to it
            a.url = storage::getStr("dav_url");
            a.user = storage::getStr("dav_user");
            a.pass = storage::getStr("dav_pass");
            while (!a.url.empty() && a.url.back() == '/') a.url.pop_back();
            const size_t slash = a.url.rfind('/');
            if (slash != std::string::npos && slash > 8) a.url = a.url.substr(0, slash + 1) + "Journal/";
        }
        if (ui::prompt("Journal WebDAV folder", a.url, false, "e.g. https://HOST/remote.php/dav/files/USER/Journal/") &&
            ui::prompt("User name", a.user) && ui::prompt("Password", a.pass, true, "Stored unencrypted on this device.")) {
            dav::Url u;
            if (!a.url.empty() && !dav::parseUrl(a.url, u)) {
                ui::alert("Journal", "The address must start with https:// or http://");
            } else {
                if (a.url != account_.url) {
                    store_.detachAll();  // a different folder: upload everything there
                    store_.save();
                }
                account_ = a;
                account_.save();
                syncInfo_.clear();
                redraw();
                if (account_.configured()) sync(true, true);
                return;
            }
        }
        redraw();
    }

    // interactive: the user asked, so explain problems. mayAsk: dialogs are allowed.
    void sync(bool interactive, bool mayAsk) {
        if (!account_.configured() || !net::connected()) {
            if (interactive) {
                ui::alert("Journal", !account_.configured() ? "No sync folder yet. Press opt+a to set one up."
                                                             : "Wi-Fi is not connected.");
                redraw();
            }
            return;
        }
        DavClient::CertFn onCert = nullptr;
        if (mayAsk) {
            onCert = [](const std::string& host, const std::string& fp, bool changed) {
                std::string shown;
                for (size_t i = 0; i < fp.size(); i += 4) shown += fp.substr(i, 4) + " ";
                if (changed)
                    return ui::confirm("CERTIFICATE CHANGED",
                                       "The certificate of " + host + " is different from last time. That is normal after a "
                                       "renewal, but could also mean someone is intercepting the connection.\nSHA-256: " +
                                           shown + "\nTrust the new certificate?");
                return ui::confirm("New server", "First connection to " + host + ".\nCertificate SHA-256:\n" + shown +
                                                     "\nTrust this server?");
            };
        }
        const JournalSyncResult r = syncJournal(store_, account_, onCert, [](const std::string& m) { ui::busy(m); });
        if (r.ok) {
            const time_t t = time(nullptr);
            struct tm tmv;
            localtime_r(&t, &tmv);
            char buf[24];
            snprintf(buf, sizeof buf, "synced %02d:%02d", tmv.tm_hour, tmv.tm_min);
            syncInfo_ = buf;
        } else {
            syncInfo_ = "sync failed";
            if (interactive || mayAsk) ui::alert("Journal sync failed", r.error);
        }
        // the day being shown may have been removed on the server
        if (!store_.find(viewDate_) && !(clockValid() && viewDate_ == today()))
            viewDate_ = clockValid() ? today() : (store_.days().empty() ? "" : store_.days().back().date);
        os::refreshStatus();
        if (mayAsk) redraw();
    }

    // ------------------------------------------------------------ drawing
    struct Line {
        std::string time, text;
    };

    void redraw() {
        os::Gfx& g = os::gfx();
        const os::Rect c = os::content();
        g.fillRect(c.x, c.y, c.w, c.h, theme::bg);
        std::string title = viewDate_.empty() ? "Journal" : prettyDate(viewDate_);
        if (clockValid() && viewDate_ == today()) title += "  (today)";
        const int unsynced = store_.unsyncedCount();
        if (!account_.configured()) title += "   - no sync";
        else if (unsynced) title += "   - " + std::to_string(unsynced) + " not synced";
        else if (!syncInfo_.empty()) title += "   - " + syncInfo_;
        const os::Rect body = ui::header(g, c, title.c_str());
        const int cols = (c.w - 8) / kCw, textCols = cols - 6;
        const int rows = (body.h - kInputH - 2) / kLineH;

        std::vector<Line> lines;
        for (auto& e : journal::entries(store_.text(viewDate_))) {
            char tbuf[8];
            snprintf(tbuf, sizeof tbuf, "%02d:%02d", e.minutes / 60, e.minutes % 60);
            bool first = true;
            // each line of the entry is wrapped on its own
            size_t pos = 0;
            const std::string all = e.text;
            while (pos <= all.size()) {
                size_t nl = all.find('\n', pos);
                if (nl == std::string::npos) nl = all.size();
                std::string text = ui::displayable(all.substr(pos, nl - pos));
                pos = nl + 1;
                do {
                    size_t cut = text.size();
                    if ((int)cut > textCols) {
                        cut = text.rfind(' ', textCols);
                        if (cut == std::string::npos || cut < (size_t)textCols / 2) cut = textCols;
                    }
                    lines.push_back({first ? tbuf : "", text.substr(0, cut)});
                    first = false;
                    text.erase(0, cut);
                    while (!text.empty() && text[0] == ' ') text.erase(0, 1);
                } while (!text.empty());
            }
        }
        const int maxScroll = std::max(0, (int)lines.size() - rows);
        scroll_ = std::max(0, std::min(scroll_, maxScroll));
        const int firstLine = std::max(0, (int)lines.size() - rows - scroll_);

        g.startWrite();
        g.setFont(&fonts::AsciiFont8x16);
        g.setTextDatum(textdatum_t::top_left);
        if (lines.empty()) {
            g.setTextColor(theme::dim);
            const char* help[] = {"No entries for this day.", "", "Type and press Enter to add one.",
                                  "Left / Right: other days", "opt+d: list of days", "opt+u: remove the last entry",
                                  "opt+s: sync    opt+a: sync folder"};
            int y = body.y + 4;
            for (const char* h : help) {
                g.drawString(h, body.x + 4, y);
                y += kLineH;
            }
        }
        for (int i = 0; i < rows && firstLine + i < (int)lines.size(); i++) {
            const Line& l = lines[firstLine + i];
            const int y = body.y + 2 + i * kLineH;
            if (!l.time.empty()) {
                g.setTextColor(dr::purple);
                g.drawString(l.time.c_str(), body.x + 4, y);
            }
            g.setTextColor(theme::text);
            g.drawString(l.text.c_str(), body.x + 4 + 6 * kCw, y);
        }
        if (scroll_ > 0) {
            const int ay = c.y + c.h - kInputH - 6;
            g.fillTriangle(c.x + c.w - 10, ay - 6, c.x + c.w - 4, ay - 6, c.x + c.w - 7, ay, theme::dim);
        }
        g.endWrite();
        drawInput();
    }

    void drawInput() {
        os::Gfx& g = os::gfx();
        const os::Rect c = os::content();
        const int y = c.y + c.h - kInputH;
        g.startWrite();
        g.fillRect(c.x, y, c.w, kInputH, theme::panel);
        g.setFont(&fonts::AsciiFont8x16);
        g.setTextDatum(textdatum_t::top_left);
        if (!hint_.empty() && input_.empty()) {
            g.setTextColor(theme::warn);
            g.drawString(hint_.c_str(), c.x + 4, y + 2);
        } else {
            const int cols = (c.w - 8) / kCw;
            std::string shown = "> " + input_ + "_";
            if ((int)shown.size() > cols) shown = shown.substr(shown.size() - cols);
            g.setTextColor(theme::text);
            g.drawString(shown.c_str(), c.x + 4, y + 2);
        }
        g.endWrite();
    }

    JournalStore store_;
    JournalAccount account_;
    std::string viewDate_, input_, hint_, syncInfo_, status_;
    int scroll_ = 0;
    bool autoSynced_ = false;
};

OS_REGISTER_APP(JournalApp, "Journal", "Timestamped daily notes, WebDAV sync", theme::dracula::yellow, icons::journal, 22);

}  // namespace
