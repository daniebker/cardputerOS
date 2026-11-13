#include "kernel.h"

#include <M5Cardputer.h>
#include <esp_system.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../hal/display.h"
#include "../hal/keyboard.h"
#include "icons.h"
#include "net.h"
#include "power.h"
#include "secrets.h"
#include "storage.h"
#include "theme.h"
#include "ui.h"

namespace os {

namespace {

struct Slot {
    AppInfo info;
    App* inst = nullptr;
};

// Function-local static: safe to use from static registrars in other files.
std::vector<Slot>& slots() {
    static std::vector<Slot> s;
    return s;
}

int foreground = -1;  // index into slots(), -1 = launcher
bool homePending = false;
uint32_t lastStatusCheck = 0, lastNetTick = 0, lastBattery = 0;
int batteryLevel = -1;

void launch(int index);
void closeApp(int index);

// ------------------------------------------------------------ launcher
// Home screen: a grid of app icons. Apps registered with a folder are grouped
// behind one folder icon; Enter opens it, Esc / Backspace goes back.
class Launcher {
public:
    void resume() {
        build();
        Gfx& g = gfx();
        g.startWrite();
        Rect c = content();
        g.fillRect(c.x, c.y, c.w, c.h, theme::bg);
        g.endWrite();
        scrollToSelection();
        drawAll();
    }

    // The folder shown ("" = the home screen itself).
    const std::string& folder() const { return folder_; }

    // Back to the top level (pressing "home" while already on the home screen).
    void toTop() {
        if (folder_.empty()) return;
        leaveFolder();
    }

    void onKey(const key::Event& ev) {
        const int n = (int)entries_.size();
        if (!folder_.empty() && (ev.is(key::Esc) || ev.is(key::Backspace) || ev.is('`'))) {
            leaveFolder();
            return;
        }
        if (n == 0) return;
        const Entry& e = entries_[sel_];
        if (ev.is('x') && e.slot >= 0 && slots()[e.slot].inst) {
            closeApp(e.slot);
            resume();
            return;
        }
        if (ev.is(key::Enter)) {
            if (e.slot >= 0) {
                launch(e.slot);
            } else {
                topSel_ = sel_;
                topRow0_ = topRow_;
                folder_ = e.folder;
                sel_ = topRow_ = 0;
                resume();
            }
            return;
        }
        int next = sel_;
        if (ev.is(key::Left) || ev.is(',') || ev.is('h')) next--;
        else if (ev.is(key::Right) || ev.is('/') || ev.is('l') || ev.is(key::Tab)) next++;
        else if (ui::isUp(ev)) next -= kCols;
        else if (ui::isDown(ev)) next += kCols;
        else if (ev.is(key::Home)) next = 0;
        else if (ev.is(key::End)) next = n - 1;
        else return;
        if (ev.is(key::Tab)) next = (next + n) % n;  // Tab cycles
        // moving down from the last full row lands on the last app
        if (next >= n && sel_ / kCols < (n - 1) / kCols) next = n - 1;
        if (next < 0 || next >= n || next == sel_) return;
        const int old = sel_, oldTop = topRow_;
        sel_ = next;
        scrollToSelection();
        if (topRow_ != oldTop) {
            drawAll();
        } else {
            Gfx& g = gfx();
            g.startWrite();
            drawTile(g, old);
            drawTile(g, sel_);
            g.endWrite();
            drawFooter();
        }
    }

private:
    static constexpr int kCols = 3, kRows = 2;
    static constexpr int kFooterH = 18, kIcon = 56;

    struct Entry {
        int slot;            // index into slots(), or -1 for a folder
        std::string folder;  // folder name (slot == -1)
    };

    // The entries of the current folder, in home screen order.
    void build() {
        entries_.clear();
        for (int i = 0; i < (int)slots().size(); i++) {
            const char* f = slots()[i].info.folder;
            if (folder_.empty()) {
                if (!f) {
                    entries_.push_back({i, std::string()});
                } else {
                    bool seen = false;
                    for (auto& e : entries_) seen = seen || (e.slot < 0 && e.folder == f);
                    if (!seen) entries_.push_back({-1, f});
                }
            } else if (f && folder_ == f) {
                entries_.push_back({i, std::string()});
            }
        }
        const int n = (int)entries_.size();
        if (sel_ >= n) sel_ = n - 1;
        if (sel_ < 0) sel_ = 0;
    }

    void leaveFolder() {
        folder_.clear();
        sel_ = topSel_;
        topRow_ = topRow0_;
        resume();
    }

    // Apps in folder `name`.
    std::vector<int> members(const std::string& name) const {
        std::vector<int> out;
        for (int i = 0; i < (int)slots().size(); i++)
            if (slots()[i].info.folder && name == slots()[i].info.folder) out.push_back(i);
        return out;
    }

    void scrollToSelection() {
        const int row = sel_ / kCols;
        if (row < topRow_) topRow_ = row;
        if (row >= topRow_ + kRows) topRow_ = row - kRows + 1;
    }

    Rect tileRect(int index) const {
        Rect c = content();
        const int tw = c.w / kCols, th = (c.h - kFooterH) / kRows;
        return {c.x + (index % kCols) * tw, c.y + (index / kCols - topRow_) * th, tw, th};
    }

    void drawTile(Gfx& g, int index) {
        const int row = index / kCols - topRow_;
        if (index < 0 || index >= (int)entries_.size() || row < 0 || row >= kRows) return;
        const Entry& e = entries_[index];
        const Rect t = tileRect(index);
        const bool sel = index == sel_;
        const uint32_t bg = sel ? theme::select : theme::bg;
        g.fillRect(t.x, t.y, t.w, t.h, theme::bg);
        if (sel) {
            g.fillRoundRect(t.x + 4, t.y + 4, t.w - 8, t.h - 8, 8, bg);
            g.drawRoundRect(t.x + 4, t.y + 4, t.w - 8, t.h - 8, 8, theme::accent);
        }
        const int ix = t.x + (t.w - kIcon) / 2, iy = t.y + 14;
        const char* name;
        bool running = false;
        if (e.slot >= 0) {
            const Slot& s = slots()[e.slot];
            name = s.info.name;
            running = s.inst != nullptr;
            if (s.info.icon) s.info.icon(g, ix, iy, kIcon, s.info.color, bg);
            else icons::letter(g, ix, iy, kIcon, s.info.color, bg, s.info.name[0]);
        } else {
            name = e.folder.c_str();
            uint32_t colors[4];
            int count = 0;
            for (int m : members(e.folder)) {
                if (count < 4) colors[count++] = slots()[m].info.color;
                running = running || slots()[m].inst;
            }
            icons::folder(g, ix, iy, kIcon, theme::dracula::comment, bg, colors, count);
        }
        g.setFont(theme::fontUi());
        if (g.textWidth(name) > t.w - 10) g.setFont(theme::fontSmall());  // long names
        g.setTextDatum(textdatum_t::top_left);
        g.setTextColor(sel ? theme::text : theme::dim);
        const int tw = std::min<int>(g.textWidth(name), t.w - 10);
        ui::drawClipped(g, name, t.x + (t.w - tw) / 2, iy + kIcon + 8, t.w - 10);
        if (running) g.fillCircle(t.x + t.w - 16, t.y + 16, 4, theme::ok);  // running
    }

    void drawFooter() {
        Gfx& g = gfx();
        std::string hint;
        if (!folder_.empty()) hint = folder_ + " /  ";
        if (!entries_.empty()) {
            const Entry& e = entries_[sel_];
            if (e.slot >= 0) {
                const Slot& s = slots()[e.slot];
                hint += std::string(s.info.description ? s.info.description : s.info.name) + "   -   Enter: open";
                if (s.inst) hint += "   x: close";
            } else {
                std::string names;
                for (int m : members(e.folder)) names += (names.empty() ? "" : ", ") + std::string(slots()[m].info.name);
                hint += names + "   -   Enter: open";
            }
        }
        if (!folder_.empty()) hint += "   Esc: back";
        ui::footer(g, content(), hint.c_str());
        // more rows above / below
        Rect c = content();
        const int rows = ((int)entries_.size() + kCols - 1) / kCols;
        const int ax = c.x + c.w - 12, ay = c.y + c.h - kFooterH / 2;
        if (topRow_ > 0) g.fillTriangle(ax - 4, ay - 1, ax + 4, ay - 1, ax, ay - 6, theme::dim);
        if (topRow_ + kRows < rows) g.fillTriangle(ax - 4, ay + 1, ax + 4, ay + 1, ax, ay + 6, theme::dim);
    }

    void drawAll() {
        Gfx& g = gfx();
        Rect c = content();
        g.startWrite();
        g.fillRect(c.x, c.y, c.w, c.h - kFooterH, theme::bg);
        if (entries_.empty()) {
            g.setFont(theme::fontUi());
            g.setTextColor(theme::dim);
            g.setTextDatum(textdatum_t::top_left);
            g.drawString("No apps installed", c.x + 10, c.y + 10);
        }
        for (int i = topRow_ * kCols; i < (topRow_ + kRows) * kCols; i++) drawTile(g, i);
        g.endWrite();
        drawFooter();
    }

    std::vector<Entry> entries_;
    std::string folder_;
    int sel_ = 0, topRow_ = 0;
    int topSel_ = 0, topRow0_ = 0;  // position on the home screen while a folder is open
};
Launcher launcher;

void clearContent() {
    Rect c = content();
    gfx().fillRect(c.x, c.y, c.w, c.h, theme::bg);
}

void showForeground() {
    refreshStatus();
    clearContent();
    if (foreground < 0) launcher.resume();
    else slots()[foreground].inst->onResume();
}

void goHome() {
    homePending = false;
    if (foreground < 0) {
        launcher.toTop();  // "home" on the home screen leaves an open folder
        return;
    }
    slots()[foreground].inst->onPause();
    foreground = -1;
    showForeground();
}

void launch(int index) {
    if (index < 0 || index >= (int)slots().size()) return;
    Slot& s = slots()[index];
    if (!s.inst) {
        s.inst = s.info.create();
        s.inst->onStart();
    }
    foreground = index;
    refreshStatus();
    clearContent();
    if (!s.inst->finished()) s.inst->onResume();
}

void closeApp(int index) {
    Slot& s = slots()[index];
    if (!s.inst) return;
    if (foreground == index) {
        s.inst->onPause();
        foreground = -1;
    }
    delete s.inst;
    s.inst = nullptr;
}

// ------------------------------------------------------------ status bar
// ------------------------------------------------------------ status screen
// All status lives on the Cardputer's built-in 240x135 screen; the external
// display is left entirely to the apps.
struct Status {
    std::string app, appStatus, wifi, ip, battery, clock, notice;
    uint32_t wifiColor = 0;
    int signal = 0;  // 0..3 bars
    bool operator==(const Status& o) const {
        return app == o.app && appStatus == o.appStatus && wifi == o.wifi && ip == o.ip && battery == o.battery &&
               clock == o.clock && notice == o.notice && wifiColor == o.wifiColor && signal == o.signal;
    }
};
Status shownStatus;
bool statusValid = false;

Status currentStatus() {
    Status st;
    st.app = foreground < 0 ? (launcher.folder().empty() ? "Home" : launcher.folder()) : slots()[foreground].info.name;
    if (!secrets::unlocked()) st.app = "Locked";
    if (foreground >= 0) {
        const char* t = slots()[foreground].inst->statusText();
        if (t) st.appStatus = t;
    }
    if (net::connected()) {
        st.wifi = net::ssid();
        st.ip = net::ip();
        const int r = net::rssi();
        st.signal = r > -60 ? 3 : (r > -75 ? 2 : 1);
        st.wifiColor = theme::ok;
    } else if (!net::ssid().empty()) {
        st.wifi = "connecting to " + net::ssid();
        st.wifiColor = theme::warn;
    } else {
        st.wifi = net::started() ? "no Wi-Fi" : "Wi-Fi starting";
        st.wifiColor = theme::dim;
    }
    for (auto& sl : slots()) {
        if (!sl.inst) continue;
        const char* n = sl.inst->notice();
        if (n && *n && st.notice.empty()) st.notice = n;
    }
    if (batteryLevel >= 0) st.battery = std::to_string(batteryLevel) + "%";
    time_t now = time(nullptr);
    if (now > 1700000000) {
        struct tm tmv;
        localtime_r(&now, &tmv);
        char buf[8];
        snprintf(buf, sizeof buf, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        st.clock = buf;
    }
    return st;
}

void clipped(lgfx::LovyanGFX& d, const std::string& text, int x, int y, int maxW) { ui::drawClipped(d, text, x, y, maxW); }

void drawStatus(const Status& st) {
    auto& d = M5Cardputer.Display;
    const int w = d.width(), h = d.height();
    d.startWrite();
    d.fillScreen(theme::bg);
    d.setTextDatum(textdatum_t::top_left);

    // top: clock and battery
    d.setFont(&fonts::DejaVu18);
    d.setTextColor(theme::text);
    const int clockW = st.clock.empty() ? 0 : d.textWidth(st.clock.c_str());
    if (!st.clock.empty()) d.drawString(st.clock.c_str(), w - clockW - 6, 4);
    if (!st.battery.empty()) {
        // small battery icon + percentage, left of the clock
        d.setFont(&fonts::DejaVu12);
        const int tw = d.textWidth(st.battery.c_str());
        const int bx = w - clockW - 16 - tw - 22;
        const int level = atoi(st.battery.c_str());
        const uint32_t col = level <= 15 ? theme::error : (level <= 30 ? theme::warn : theme::ok);
        d.drawRect(bx, 9, 16, 9, theme::dim);
        d.fillRect(bx + 16, 11, 2, 5, theme::dim);
        d.fillRect(bx + 2, 11, std::max(1, 12 * level / 100), 5, col);
        d.setTextColor(theme::dim);
        d.drawString(st.battery.c_str(), bx + 22, 8);
    }
    // app name
    d.setFont(&fonts::DejaVu18);
    d.setTextColor(theme::accent);
    clipped(d, st.app, 6, 4, w / 2 - 10);

    // app status
    d.setFont(&fonts::DejaVu12);
    d.setTextColor(theme::text);
    if (!st.appStatus.empty()) clipped(d, st.appStatus, 6, 30, w - 12);
    d.drawFastHLine(6, 50, w - 12, theme::select);

    // network
    const int ny = 58;
    for (int i = 0; i < 3; i++) {
        const int bh = 4 + 4 * i;
        const uint32_t c = i < st.signal ? st.wifiColor : theme::select;
        d.fillRect(6 + i * 5, ny + 12 - bh, 3, bh, c);
    }
    d.setTextColor(st.wifiColor);
    clipped(d, st.wifi, 26, ny, w - 32);
    if (!st.ip.empty()) {
        d.setTextColor(theme::dim);
        d.drawString(st.ip.c_str(), 26, ny + 18);
    }

    // key hints
    // bottom line: a notice from an app if there is one, otherwise the key hints
    if (!st.notice.empty()) {
        d.setFont(&fonts::DejaVu12);
        d.setTextColor(theme::warn);
        clipped(d, st.notice, 6, h - 18, w - 12);
    } else {
        d.setFont(&fonts::DejaVu9);
        d.setTextColor(theme::dim);
        clipped(d, "opt+` home   opt+r redraw screen", 6, h - 14, w - 12);
    }
    d.endWrite();
}

void markStage(char stage);  // defined below
void showForeground();

// Light sleep until a key is pressed (battery saving, see power.h).
void goToSleep(unsigned timerWakeMs = 0) {
    if (foreground >= 0) slots()[foreground].inst->onPause();  // apps save their work
    power::sleepNow(timerWakeMs);
    markStage('S');
    statusValid = false;
    showForeground();
}

// Sets the external display up again and repaints everything (opt+r).
void reinitDisplay() {
    hal::display.init();
    displaySettingsChanged();
}

void services(bool maySleep) {
    const uint32_t now = millis();
    if (power::tick() == power::Action::Sleep) {
        bool keepAwake = false;
        for (auto& sl : slots())
            if (sl.inst && sl.inst->keepAwake()) keepAwake = true;
        // not while a dialog is open (maySleep == false): it would be wiped by the repaint
        if (maySleep && !keepAwake) {
            goToSleep();
            return;
        }
    }
    markStage('L');
    if (now > 5000) markStage('5');
    if (now > 30000) markStage('T');
    // Wi-Fi needs the saved passwords: wait until the secrets are unlocked
    if (!net::started() && now > 4000 && secrets::unlocked()) {
        net::begin();
        markStage('W');
    }
    if (now - lastNetTick >= 100) {
        lastNetTick = now;
        net::tick();
    }
    if (batteryLevel < 0 || now - lastBattery >= 15000) {
        lastBattery = now;
        batteryLevel = M5.Power.getBatteryLevel();
    }
    if (now - lastStatusCheck >= 500) {
        lastStatusCheck = now;
        refreshStatus();
    }
    for (int i = 0; i < (int)slots().size(); i++) {
        if (i != foreground && slots()[i].inst) slots()[i].inst->onBackgroundTick();
    }
}

}  // namespace

AppRegistrar::AppRegistrar(const AppInfo& info) {
    Slot s;
    s.info = info;
    // keep the home screen order (registration order itself is arbitrary)
    auto& v = slots();
    auto pos = v.begin();
    while (pos != v.end() && pos->info.order <= info.order) ++pos;
    v.insert(pos, s);
}

#ifdef SCREENSHOT_DEBUG
// Debug build: everything is drawn into this canvas, which can be dumped over USB.
static M5Canvas debugCanvas;
Gfx& gfx() {
    if (!debugCanvas.getBuffer()) {
        debugCanvas.setColorDepth(8);
        debugCanvas.createSprite(hal::display.width(), hal::display.height());
    }
    return debugCanvas;
}

static void dumpScreenshot() {
    Gfx& g = gfx();
    Serial.printf("\nSHOT %d %d\n", g.width(), g.height());
    Serial.flush();
    const uint8_t* p = (const uint8_t*)debugCanvas.getBuffer();
    const size_t n = (size_t)g.width() * g.height();
    for (size_t off = 0; off < n; off += 256) {
        Serial.write(p + off, std::min<size_t>(256, n - off));
        Serial.flush();
    }
    Serial.print("\nEND\n");
}
#else
Gfx& gfx() { return hal::display; }
#endif

Rect content() {
    return {0, 0, hal::display.width(), hal::display.height()};
}

void refreshStatus() {
    Status st = currentStatus();
    if (statusValid && st == shownStatus) return;
    shownStatus = st;
    statusValid = true;
    drawStatus(st);
}

void displaySettingsChanged() {
    hal::display.setFlipped(storage::getInt("flip", 0) != 0);
    hal::display.fillScreen(theme::bg);
    showForeground();
}

bool modalPoll(key::Event& ev) {
#ifdef SCREENSHOT_DEBUG
    if (hal::takeScreenshotRequest()) dumpScreenshot();
#endif
    if (homePending) {
        ev = key::Event();
        ev.code = key::Esc;
        return true;
    }
    services(false);
    if (hal::keyboardPoll(ev)) {
        power::activity();
        if (ev.code == key::OsHome) {
            homePending = true;
            ev.code = key::Esc;
        }
        return true;
    }
    delay(2);
    return false;
}

void idle() {
    services(false);
    delay(2);
}

namespace {
const char* resetName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_PANIC: return "CRASH";
        case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "WATCHDOG";
        case ESP_RST_SW: return "restart";
        case ESP_RST_DEEPSLEEP: return "wake";
        case ESP_RST_EXT: return "reset-pin";
        default: return "other";
    }
}

// Boot log entries look like "power-on:DL5W" - the reset reason, then the stages
// this boot reached: D display ready, L main loop running, 5 five seconds up,
// W Wi-Fi started, T thirty seconds up. Shows how far a boot got before power was lost.
std::string bootStages;
void markStage(char stage) {
    if (bootStages.find(stage) != std::string::npos) return;
    bootStages.push_back(stage);
    std::string log = storage::getStr("bootlog");
    size_t end = log.find(',');
    std::string first = log.substr(0, end);
    first = first.substr(0, first.find(':')) + ":" + bootStages;
    storage::putStr("bootlog", first + (end == std::string::npos ? "" : log.substr(end)));
}

// Keeps the reasons of the last 12 boots in NVS (newest first).
void recordBoot() {
    std::string log = storage::getStr("bootlog");
    log = std::string(resetName(esp_reset_reason())) + (log.empty() ? "" : "," + log);
    int commas = 0;
    for (size_t i = 0; i < log.size(); i++) {
        if (log[i] == ',' && ++commas == 12) {
            log.resize(i);
            break;
        }
    }
    storage::putStr("bootlog", log);
    Serial.printf("\nBOOTLOG %s\n", log.c_str());
}
}  // namespace

std::string bootHistory() { return storage::getStr("bootlog"); }

namespace {
void splash(const char* subtitle) {
    Gfx& g = gfx();
    const Rect c = content();
    g.startWrite();
    g.fillRect(c.x, c.y, c.w, c.h, theme::bg);
    g.setTextDatum(textdatum_t::top_center);
    g.setFont(theme::fontBig());
    g.setTextColor(theme::accent);
    g.drawString("cardputerOS", c.x + c.w / 2, c.y + 14);
    g.setFont(theme::fontSmall());
    g.setTextColor(theme::dim);
    g.drawString(subtitle, c.x + c.w / 2, c.y + 40);
    g.endWrite();
}

// First start with encryption: choose the passphrase, then encrypt what is stored.
void choosePassphrase() {
    splash("Set up encryption");
    ui::alert("Choose a passphrase",
              "Saved Wi-Fi, SSH and WebDAV passwords and the SSH key are stored encrypted. "
              "Choose a passphrase to protect them; it is asked for each time the Cardputer starts.\n\n"
              "If you forget it, these have to be entered again.");
    for (int attempt = 0; attempt < 3; attempt++) {
        std::string pass;
        homePending = false;
        while (!secrets::askNewPassphrase("New passphrase", pass)) homePending = false;
        splash("Set up encryption");
        ui::busy("Encrypting...");
        const bool ok = secrets::setUp(pass);
        pass.assign(pass.size(), '\0');
        if (ok) return;
        ui::alert("Encryption", "The passphrase could not be saved.");
    }
    // give up: the device works, but nothing that needs a secret does (not even Wi-Fi)
}

// True if the user chose to delete the secrets and set a new passphrase.
bool forgotPassphrase() {
    homePending = false;
    if (!ui::confirm("Forgot the passphrase?",
                     "The saved Wi-Fi, SSH and WebDAV passwords and the SSH key cannot be recovered "
                     "without it and will be deleted. Notes, journal and other settings are kept.\n\n"
                     "Delete them and choose a new passphrase?"))
        return false;
    secrets::erase();
    choosePassphrase();
    return true;
}

// Brute force protection: after too many wrong passphrases, count down until the
// next try is allowed. False if Esc was pressed meanwhile.
bool waitLockout() {
    uint32_t shown = 0;
    bool drawn = false;
    for (;;) {
        const uint32_t s = secrets::waitSeconds();
        if (s == 0) return true;
        if (!drawn || s != shown) {
            if (!drawn) splash("Locked   -   Esc: forgot the passphrase");
            drawn = true;
            shown = s;
            char buf[48];
            snprintf(buf, sizeof buf, "Too many wrong tries. Wait %u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
            ui::busy(buf);
        }
        key::Event ev;
        if (modalPoll(ev)) {
            homePending = false;
            if (ev.code == key::Esc) return false;
        }
    }
}

// Asks for the passphrase until it is right; nothing can read the secrets before.
void unlockAtBoot() {
    if (!secrets::isSetUp()) {
        choosePassphrase();
        return;
    }
    for (;;) {
        homePending = false;
        if (!waitLockout()) {
            if (forgotPassphrase()) return;
            continue;
        }
        splash("Locked");
        std::string pass;
        const uint32_t failed = secrets::failedAttempts();
        std::string hint = "Enter your passphrase.   Esc: forgot it";
        if (failed) hint = std::to_string(failed) + " wrong " + (failed == 1 ? "try" : "tries") + ".   Esc: forgot it";
        if (!ui::prompt("Unlock", pass, true, hint.c_str())) {
            if (forgotPassphrase()) return;
            continue;
        }
        ui::busy("Unlocking...");
        const bool ok = secrets::unlock(pass);
        pass.assign(pass.size(), '\0');
        if (ok) return;
        homePending = false;
        if (secrets::waitSeconds() == 0) ui::alert("Unlock", "Wrong passphrase.");
    }
}
}  // namespace

void begin() {
    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    cfg.internal_spk = false;  // not used yet; saves RAM for Wi-Fi + SSH
    cfg.internal_mic = false;
    cfg.internal_imu = false;
    M5Cardputer.begin(cfg, true);
    // M5GFX already picks the right orientation for the built-in screen; only
    // intervene if it somehow came up in portrait.
    if (M5Cardputer.Display.width() < M5Cardputer.Display.height()) {
        M5Cardputer.Display.setRotation(M5Cardputer.Display.getRotation() ^ 1);
    }
    M5Cardputer.Display.setBrightness(70);

    storage::begin();
    recordBoot();
    net::applyTimezone();  // local time is right even before Wi-Fi/NTP
    hal::display.begin(storage::getInt("flip", 0) != 0);
    hal::display.fillScreen(theme::bg);
    hal::display.setFont(theme::fontBig());
    hal::display.setTextColor(theme::accent);
    hal::display.setTextDatum(textdatum_t::middle_center);
    hal::display.drawString("cardputerOS", hal::display.width() / 2, hal::display.height() / 2);
    {
        Status st;
        st.app = "Starting...";
        drawStatus(st);
    }

    storage::mountFiles();
    hal::keyboardBegin();
    markStage('D');
    power::activity();
    unlockAtBoot();
    homePending = false;
    // Wi-Fi starts a few seconds after boot (see services()), once the UI is up.

    hal::display.fillScreen(theme::bg);
    showForeground();
}

void loopOnce() {
    services(true);

#ifdef SCREENSHOT_DEBUG
    if (hal::takeScreenshotRequest()) dumpScreenshot();
    if (hal::takeSleepTestRequest()) goToSleep(6000);
#endif
    key::Event ev;
    while (!homePending && hal::keyboardPoll(ev)) {
        power::activity();
        if (ev.code == key::OsHome) {
            homePending = true;
        } else if (ev.isOpt('r')) {
            reinitDisplay();
        } else if (foreground < 0) {
            launcher.onKey(ev);
        } else {
            slots()[foreground].inst->onKey(ev);
        }
    }
    if (homePending) goHome();

    if (foreground >= 0) {
        App* app = slots()[foreground].inst;
        app->onTick();
        if (homePending) goHome();
        if (foreground >= 0 && slots()[foreground].inst->finished()) {
            closeApp(foreground);
            showForeground();
        }
    }
    delay(1);
}

}  // namespace os
