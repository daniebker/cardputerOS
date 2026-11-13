// Wardrive: lists the Wi-Fi networks and Bluetooth LE devices around and, if an
// SD card is inserted, logs every device the first time it is seen in a session to
// a CSV file (WiGLE format; the device has no GPS, so there is no location).
// Without a card nothing is saved. Scanning continues while the app is in the
// background; it stops when the app is closed or paused.
#include <WiFi.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../../hal/sdcard.h"
#include "../../os/bluetooth.h"
#include "../../os/icons.h"
#include "../../os/kernel.h"
#include "../../os/net.h"
#include "../../os/storage.h"
#include "../../os/theme.h"
#include "../../os/ui.h"
#include "ble_scanner.h"
#include "wardrive.h"

namespace {

namespace dr = theme::dracula;
using wardrive::Device;
using wardrive::Kind;
using wardrive::Sort;

constexpr uint32_t kInRangeMs = 30000;  // "in range": seen within this time
constexpr uint32_t kRefreshMs = 1000;   // list repaint interval
constexpr uint32_t kFlushMs = 15000;    // SD write interval
constexpr size_t kFlushBytes = 4096;    // ... or as soon as this much is waiting
constexpr uint32_t kWifiDwellMs = 120;  // per channel and scan
constexpr const char* kLogDir = "/cardputerOS/wardrive";

int64_t clockNow() {
    const time_t t = time(nullptr);
    return t > 1700000000 ? (int64_t)t : 0;
}

class WardriveApp : public os::App {
public:
    void onStart() override {
        const int s = storage::getInt("wd_sort", 0);
        sort_ = (s >= 0 && s <= 2) ? (Sort)s : Sort::Signal;
        startLog(true);
        setRunning(true);
    }

    ~WardriveApp() override {
        setRunning(false);
        flush();
    }

    void onResume() override {
        foreground_ = true;
        drawAll();
    }

    void onPause() override {
        foreground_ = false;
        flush();
    }

    // Scanning stops in sleep (the radios go off), so stay awake while it runs.
    bool keepAwake() override { return running_; }

    const char* statusText() override {
        status_ = std::to_string(table_.active(Kind::Wifi, millis(), kInRangeMs)) + "/" +
                  std::to_string(table_.total(Kind::Wifi)) + " Wi-Fi   " +
                  std::to_string(table_.active(Kind::Ble, millis(), kInRangeMs)) + "/" +
                  std::to_string(table_.total(Kind::Ble)) + " BT   " +
                  (!running_ ? "paused" : (logging_ ? "saving" : "not saving"));
        return status_.c_str();
    }

    const char* notice() override {
        if (foreground_) return nullptr;
        if (sdFailed_) return "Wardrive: SD card write failed, not saving";
        if (!running_) return nullptr;
        notice_ = "Wardriving: " + std::to_string(table_.total(Kind::Wifi)) + " Wi-Fi, " +
                  std::to_string(table_.total(Kind::Ble)) + " Bluetooth";
        return notice_.c_str();
    }

    void onTick() override {
        work();
        if (millis() - lastRefresh_ >= kRefreshMs) refresh();
    }

    void onBackgroundTick() override { work(); }

    void onKey(const key::Event& ev) override {
        os::Gfx& g = os::gfx();
        if (ev.is(key::Tab) || ev.is(key::Left) || ev.is(key::Right) || ev.is(',') || ev.is('/')) {
            tab_ = tab_ == Kind::Wifi ? Kind::Ble : Kind::Wifi;
            drawAll();
        } else if (ev.is('o')) {
            sort_ = (Sort)(((int)sort_ + 1) % 3);
            storage::putInt("wd_sort", (int)sort_);
            refresh(true);
        } else if (ev.is(' ') || ev.is('p')) {
            setRunning(!running_);
            refresh(true);
        } else if (ev.is('s')) {
            sdKey();
            drawAll();
        } else if (ev.is('n')) {
            if (ui::confirm("New session", "Clear the list and start a new log file?")) newSession();
            drawAll();
        } else if (ev.is(key::Enter)) {
            if (list_.selected >= 0 && list_.selected < (int)order_.size()) showDetails(table_.at(order_[list_.selected]));
            drawAll();
        } else if (list_.onKey(g, listRect(), ev) == ui::List::Moved) {
            selectedMac_[(int)tab_] = table_.at(order_[list_.selected]).mac;
        }
    }

private:
    // ------------------------------------------------------------ scanning
    void setRunning(bool on) {
        if (on == running_) return;
        running_ = on;
        if (on) {
            net::holdScans(true);
            nextWifiScan_ = millis();
            startBle();
        } else {
            ble_.stop();
            wifiScanning_ = false;
            net::holdScans(false);
        }
    }

    void startBle() {
        bleError_.clear();
        if (ble_.start()) return;
        const char* owner = bt::owner();
        bleError_ = owner ? std::string("Bluetooth is in use by ") + owner + " - close it to scan here"
                          : "Bluetooth could not be started (not enough memory?)";
        bleRetry_ = owner != nullptr;  // try again once the other app lets go
        nextBleRetry_ = millis() + 3000;
    }

    void work() {
        if (running_) {
            const uint32_t now = millis();
            const int64_t t = clockNow();
            if (!ble_.running() && bleRetry_ && !bt::owner() && (int32_t)(now - nextBleRetry_) >= 0) startBle();
            wifiTick(now, t);
            if (ble_.running()) {
                bleBuf_.clear();
                ble_.take(bleBuf_);
                for (auto& d : bleBuf_) seen(d, now, t);
            }
        }
        if (logging_ && !pending_.empty() && (pending_.size() >= kFlushBytes || millis() - lastFlush_ >= kFlushMs))
            flush();
    }

    void wifiTick(uint32_t now, int64_t t) {
        if (wifiScanning_) {
            const int n = WiFi.scanComplete();
            if (n == WIFI_SCAN_RUNNING) {
                if (now - wifiScanStart_ > 15000) {  // stuck: start over
                    WiFi.scanDelete();
                    wifiScanning_ = false;
                }
                return;
            }
            wifiScanning_ = false;
            for (int i = 0; i < n; i++) {
                const uint8_t* bssid = WiFi.BSSID(i);
                if (!bssid) continue;
                Device d;
                d.kind = Kind::Wifi;
                for (int k = 0; k < 6; k++) d.mac[k] = bssid[k];
                d.name = WiFi.SSID(i).c_str();
                d.rssi = (int8_t)WiFi.RSSI(i);
                d.channel = (uint8_t)WiFi.channel(i);
                d.auth = (uint8_t)WiFi.encryptionType(i);
                seen(d, now, t);
            }
            if (n >= 0) WiFi.scanDelete();
            nextWifiScan_ = now + 250;
            return;
        }
        if ((int32_t)(now - nextWifiScan_) < 0) return;
        // Wi-Fi is switched on a few seconds after boot (and off in sleep)
        if (!net::started() || !(WiFi.getMode() & WIFI_MODE_STA)) {
            nextWifiScan_ = now + 1000;
            return;
        }
        // async, include hidden networks, active scan
        if (WiFi.scanNetworks(true, true, false, kWifiDwellMs) == WIFI_SCAN_FAILED) {
            nextWifiScan_ = now + 2000;
            return;
        }
        // (also when a scan was already running, e.g. the Wi-Fi settings': its results count too)
        wifiScanning_ = true;
        wifiScanStart_ = now;
    }

    void seen(const Device& d, uint32_t now, int64_t t) {
        bool isNew = false;
        const int i = table_.observe(d, now, t, &isNew);
        if (isNew && logging_) {
            pending_ += wardrive::csvRow(table_.at(i));
            pendingRows_++;
        }
    }

    // ------------------------------------------------------------ SD card log
    std::string newLogPath() {
        char name[48];
        const time_t t = (time_t)clockNow();
        if (t) {
            struct tm tmv;
            localtime_r(&t, &tmv);
            strftime(name, sizeof name, "%Y%m%d-%H%M%S.csv", &tmv);
        } else {
            // no clock (no Wi-Fi / NTP yet): number the sessions instead
            const int seq = storage::getInt("wd_seq", 0) + 1;
            storage::putInt("wd_seq", seq);
            snprintf(name, sizeof name, "session-%04d.csv", seq);
        }
        return std::string(kLogDir) + "/" + name;
    }

    // Opens the log (creating it on first use) and writes every device known so
    // far. `quiet`: just probe for a card, no messages.
    bool startLog(bool quiet) {
        if (logPath_.empty()) logPath_ = newLogPath();
        std::string data;
        if (!hal::sdExists(logPath_.c_str())) data = wardrive::csvHeader(os::kVersion);
        int rows = 0;
        for (auto& d : table_.devices()) {
            data += wardrive::csvRow(d);
            rows++;
        }
        const hal::SdResult r = hal::sdAppendFile(logPath_.c_str(), data);
        pending_.clear();
        pendingRows_ = 0;
        lastFlush_ = millis();
        logging_ = r == hal::SdResult::Ok;
        sdFailed_ = false;
        if (logging_) rowsWritten_ += rows;
        if (!quiet && !logging_)
            ui::alert("SD card", r == hal::SdResult::NoCard ? "No SD card found. Devices are shown but not saved."
                                                            : "Could not write to the SD card.");
        return logging_;
    }

    void flush() {
        if (!logging_ || pending_.empty()) return;
        const hal::SdResult r = hal::sdAppendFile(logPath_.c_str(), pending_);
        lastFlush_ = millis();
        if (r == hal::SdResult::Ok) {
            rowsWritten_ += pendingRows_;
        } else {
            // card removed or full: carry on without saving (s tries again)
            logging_ = false;
            sdFailed_ = true;
        }
        pending_.clear();
        pendingRows_ = 0;
    }

    void sdKey() {
        if (logging_) {
            flush();
            if (logging_) {
                ui::alert("SD card", "Saving to " + logPath_ + "\n" + std::to_string(rowsWritten_) +
                                         " devices written.\n\nCSV in WiGLE format, without location.");
                return;
            }
        }
        if (startLog(false))
            ui::alert("SD card", "Saving to " + logPath_ + "\nAll devices seen so far were written.");
    }

    void newSession() {
        flush();
        table_.clear();
        order_.clear();
        selectedMac_[0] = selectedMac_[1] = wardrive::Mac{};
        list_.selected = 0;
        logPath_.clear();
        rowsWritten_ = 0;
        if (logging_ || sdFailed_) startLog(true);
    }

    // ------------------------------------------------------------ drawing
    os::Rect area() {
        const os::Rect c = os::content();
        return {c.x, c.y + kHeaderH + kInfoH, c.w, c.h - kHeaderH - kInfoH - footerH_};
    }
    os::Rect listRect() { return area(); }

    void drawAll() {
        os::Gfx& g = os::gfx();
        const os::Rect c = os::content();
        g.startWrite();
        g.fillRect(c.x, c.y, c.w, c.h, theme::bg);
        g.endWrite();
        const os::Rect rest =
            ui::footer(g, c, "Tab: Wi-Fi/BT   o: sort   Enter: info   Space: pause   s: SD   n: new");
        footerH_ = c.h - rest.h;
        shownHeader_.clear();
        shownInfo_.clear();
        refresh(true);
    }

    // Rebuilds the list from the table and repaints what changed.
    void refresh(bool full = false) {
        lastRefresh_ = millis();
        if (!foreground_) return;
        const uint32_t now = millis();
        os::Gfx& g = os::gfx();
        drawHeader(g);
        drawInfo(g, now);

        order_ = table_.sorted(tab_, sort_, now, kInRangeMs);
        std::vector<ui::ListItem> items;
        items.reserve(order_.size());
        int sel = std::min<int>(list_.selected, (int)order_.size() - 1);
        for (int k = 0; k < (int)order_.size(); k++) {
            const Device& d = table_.at(order_[k]);
            if (d.mac == selectedMac_[(int)tab_]) sel = k;  // the selection follows its device
            items.push_back(item(d, now));
        }
        if (sel < 0) sel = 0;
        if (!order_.empty()) selectedMac_[(int)tab_] = table_.at(order_[sel]).mac;
        list_.emptyText = !running_ ? "Paused - press Space to scan"
                                    : (tab_ == Kind::Ble && !bleError_.empty() ? "Bluetooth is not available"
                                                                               : "Scanning...");
        if (full) {
            list_.items = std::move(items);
            list_.selected = sel;
            list_.draw(g, listRect());
        } else {
            list_.update(g, listRect(), std::move(items), sel);
        }
    }

    ui::ListItem item(const Device& d, uint32_t now) {
        const uint32_t age = now - d.lastMs;
        const bool inRange = age <= kInRangeMs;
        ui::ListItem it;
        it.label = ui::displayable(wardrive::displayName(d));
        std::string when = inRange ? std::to_string(d.rssi) : wardrive::ago(age) + " ago";
        if (d.kind == Kind::Wifi) {
            it.detail = when + "  ch" + std::to_string(d.channel) + "  " + wardrive::authName(d.auth);
            if (!inRange) it.color = theme::dim;
            else if (d.auth == wardrive::AuthOpen) it.color = dr::green;  // open network
            else if (d.auth == wardrive::AuthWep) it.color = dr::orange;  // broken encryption
            else if (d.name.empty()) it.color = dr::purple;               // hidden
        } else {
            const char* maker = wardrive::companyName(d.company);
            it.detail = when;
            if (maker && !d.name.empty()) it.detail += std::string("  ") + maker;
            if (!inRange) it.color = theme::dim;
            else if (d.name.empty()) it.color = dr::cyan;
        }
        return it;
    }

    void drawHeader(os::Gfx& g) {
        const std::string wifi = "Wi-Fi " + std::to_string(table_.count(Kind::Wifi));
        const std::string ble = "Bluetooth " + std::to_string(table_.count(Kind::Ble));
        std::string state;
        uint32_t stateColor;
        if (!running_) {
            state = "PAUSED";
            stateColor = dr::yellow;
        } else if (logging_) {
            state = "SD LOG";
            stateColor = dr::green;
        } else {
            state = sdFailed_ ? "SD ERROR" : "NO SD";
            stateColor = sdFailed_ ? dr::red : dr::orange;
        }
        const std::string key = wifi + "|" + ble + "|" + state + "|" + std::to_string((int)tab_);
        if (key == shownHeader_) return;
        shownHeader_ = key;

        const os::Rect c = os::content();
        g.startWrite();
        g.fillRect(c.x, c.y, c.w, kHeaderH, theme::bg);
        g.setFont(theme::fontUi());
        g.setTextDatum(textdatum_t::middle_left);
        int x = c.x + 4;
        const int y = c.y + 2, h = kHeaderH - 5;
        for (int t = 0; t < 2; t++) {
            const std::string& label = t == 0 ? wifi : ble;
            const bool sel = (int)tab_ == t;
            const int w = g.textWidth(label.c_str()) + 16;
            if (sel) {
                g.fillRoundRect(x, y, w, h, 5, theme::select);
                g.fillRect(x + 4, y + h - 2, w - 8, 2, theme::accent);
            }
            g.setTextColor(sel ? theme::text : theme::dim);
            g.drawString(label.c_str(), x + 8, y + h / 2);
            x += w + 4;
        }
        // state badge on the right
        g.setFont(theme::fontSmall());
        const int bw = g.textWidth(state.c_str()) + 12, bx = c.x + c.w - bw - 4;
        g.fillRoundRect(bx, y + 2, bw, h - 4, 4, stateColor);
        g.setTextColor(dr::darkest);
        g.setTextDatum(textdatum_t::middle_center);
        g.drawString(state.c_str(), bx + bw / 2, y + h / 2);
        g.drawFastHLine(c.x + 4, c.y + kHeaderH - 1, c.w - 8, theme::border);
        g.endWrite();
    }

    void drawInfo(os::Gfx& g, uint32_t now) {
        std::string left = std::to_string(table_.active(tab_, now, kInRangeMs)) + " in range   " +
                           std::to_string(table_.total(tab_)) + " seen   sort: " + wardrive::sortName(sort_);
        std::string right;
        uint32_t rightColor = theme::dim;
        if (tab_ == Kind::Ble && !bleError_.empty()) {
            right = bleError_;
            rightColor = theme::warn;
        } else if (logging_) {
            right = logPath_.substr(logPath_.rfind('/') + 1);
        } else if (sdFailed_) {
            right = "write failed - s: retry";
            rightColor = theme::error;
        } else {
            right = "no SD card - not saving";
            rightColor = theme::warn;
        }
        const std::string key = left + "|" + right;
        if (key == shownInfo_) return;
        shownInfo_ = key;

        const os::Rect c = os::content();
        const int y = c.y + kHeaderH;
        g.startWrite();
        g.fillRect(c.x, y, c.w, kInfoH, theme::bg);
        g.setFont(theme::fontSmall());
        g.setTextDatum(textdatum_t::top_left);
        const int rw = std::min<int>(g.textWidth(right.c_str()), c.w * 3 / 5);
        g.setTextColor(rightColor);
        ui::drawClipped(g, right, c.x + c.w - rw - 8, y + 3, rw);
        g.setTextColor(theme::dim);
        ui::drawClipped(g, left, c.x + 8, y + 3, c.w - rw - 24);
        g.endWrite();
    }

    void showDetails(const Device& d) {
        const uint32_t now = millis();
        std::string m;
        auto firstSeen = [&]() {
            std::string s = wardrive::ago(now - d.firstMs);
            s = s == "now" ? "just now" : s + " ago";
            if (d.firstTime) {
                const time_t t = (time_t)d.firstTime;
                struct tm tmv;
                localtime_r(&t, &tmv);
                char buf[12];
                strftime(buf, sizeof buf, "%H:%M:%S", &tmv);
                s = std::string(buf) + " (" + s + ")";
            }
            return s;
        };
        const std::string last = wardrive::ago(now - d.lastMs);
        const std::string signal =
            "Signal: " + std::to_string(d.rssi) + " dBm, best " + std::to_string(d.best) + " dBm";
        if (d.kind == Kind::Wifi) {
            m = "SSID: " + (d.name.empty() ? std::string("(hidden)") : ui::displayable(d.name)) + "\n";
            m += "BSSID: " + wardrive::macString(d.mac) + "\n";
            m += "Channel " + std::to_string(d.channel) + ", " + wardrive::authName(d.auth) + "\n";
        } else {
            m = "Name: " + (d.name.empty() ? std::string("(none)") : ui::displayable(d.name)) + "\n";
            m += "Address: " + wardrive::macString(d.mac) + (d.addrType ? " (random)" : " (public)") + "\n";
            if (d.company != wardrive::kNoCompany) {
                char id[8];
                snprintf(id, sizeof id, "0x%04X", d.company);
                const char* maker = wardrive::companyName(d.company);
                m += std::string("Maker: ") + (maker ? std::string(maker) + " (" + id + ")" : std::string(id)) + "\n";
            }
        }
        m += signal + "\n";
        m += "First seen: " + firstSeen() + "\n";
        m += "Last seen: " + (last == "now" ? std::string("now") : last + " ago") + ", " +
             std::to_string(d.sightings) + " times";
        ui::alert(d.kind == Kind::Wifi ? "Wi-Fi network" : "Bluetooth device", m);
    }

    static constexpr int kHeaderH = 26, kInfoH = 16;

    wardrive::Table table_;
    BleScanner ble_;
    std::vector<Device> bleBuf_;
    bool running_ = false, foreground_ = false;
    std::string bleError_;
    bool bleRetry_ = false;
    uint32_t nextBleRetry_ = 0;

    bool wifiScanning_ = false;
    uint32_t wifiScanStart_ = 0, nextWifiScan_ = 0;

    std::string logPath_, pending_;
    int pendingRows_ = 0, rowsWritten_ = 0;
    bool logging_ = false, sdFailed_ = false;
    uint32_t lastFlush_ = 0;

    Kind tab_ = Kind::Wifi;
    Sort sort_ = Sort::Signal;
    ui::List list_;
    std::vector<int> order_;  // table indices in list order
    wardrive::Mac selectedMac_[2] = {};
    uint32_t lastRefresh_ = 0;
    int footerH_ = 18;
    std::string shownHeader_, shownInfo_, status_, notice_;
};

OS_REGISTER_APP(WardriveApp, "Wardrive", "Log Wi-Fi networks and Bluetooth devices", theme::dracula::orange,
                icons::radar, 27);

}  // namespace
