#include "kernel.h"

#include <M5Cardputer.h>
#include <esp_system.h>
#include <time.h>

#include <string>
#include <vector>

#include "../hal/display.h"
#include "../hal/keyboard.h"
#include "net.h"
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
class Launcher {
public:
    void resume() {
        rebuild();
        Gfx& g = gfx();
        Rect r = ui::footer(g, content(), "Enter: open    x: close app    opt+` or G0: home");
        listRect_ = ui::header(g, r, "cardputerOS");
        list_.draw(g, listRect_);
    }
    void onKey(const key::Event& ev) {
        Gfx& g = gfx();
        if (ev.is('x') && !list_.items.empty() && slots()[list_.selected].inst) {
            closeApp(list_.selected);
            resume();
            return;
        }
        if (list_.onKey(g, listRect_, ev) == ui::List::Activated) launch(list_.selected);
    }

private:
    void rebuild() {
        list_.items.clear();
        list_.emptyText = "No apps installed";
        for (auto& s : slots()) {
            ui::ListItem it;
            it.label = s.info.name;
            it.detail = s.inst ? "running" : (s.info.description ? s.info.description : "");
            it.color = s.info.color;
            list_.items.push_back(it);
        }
        list_.clampSelection();
    }
    ui::List list_;
    Rect listRect_{0, 0, 0, 0};
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
    if (foreground < 0) return;
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
    std::string app, appStatus, wifi, ip, battery, clock;
    uint32_t wifiColor = 0;
    int signal = 0;  // 0..3 bars
    bool operator==(const Status& o) const {
        return app == o.app && appStatus == o.appStatus && wifi == o.wifi && ip == o.ip && battery == o.battery &&
               clock == o.clock && wifiColor == o.wifiColor && signal == o.signal;
    }
};
Status shownStatus;
bool statusValid = false;

Status currentStatus() {
    Status st;
    st.app = foreground < 0 ? "Home" : slots()[foreground].info.name;
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
    d.setFont(&fonts::DejaVu9);
    d.setTextColor(theme::dim);
    clipped(d, "opt+` home   opt+r redraw screen", 6, h - 14, w - 12);
    d.endWrite();
}

void markStage(char stage);  // defined below

// Sets the external display up again and repaints everything (opt+r).
void reinitDisplay() {
    hal::display.init();
    displaySettingsChanged();
}

void services() {
    const uint32_t now = millis();
    markStage('L');
    if (now > 5000) markStage('5');
    if (now > 30000) markStage('T');
    if (!net::started() && now > 4000) {
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
    slots().push_back(s);
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
    services();
    if (hal::keyboardPoll(ev)) {
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
    services();
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
    // Wi-Fi starts a few seconds after boot (see services()), once the UI is up.

    hal::display.fillScreen(theme::bg);
    showForeground();
}

void loopOnce() {
    services();

#ifdef SCREENSHOT_DEBUG
    if (hal::takeScreenshotRequest()) dumpScreenshot();
#endif
    key::Event ev;
    while (!homePending && hal::keyboardPoll(ev)) {
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
