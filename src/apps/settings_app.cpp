// Settings: Wi-Fi, display, terminal font, time zone, about.
#include <WiFi.h>

#include <cmath>
#include <cstdlib>
#include <string>

#include "../os/kernel.h"
#include "../os/icons.h"
#include "../os/net.h"
#include "../os/power.h"
#include "../os/storage.h"
#include "../os/term_view.h"
#include "../os/theme.h"
#include "../os/ui.h"

namespace {

class SettingsApp : public os::App {
public:
    void onResume() override { draw(); }

    void onKey(const key::Event& ev) override {
        if (page_ == Page::Main) mainKey(ev);
        else wifiKey(ev);
    }

private:
    enum class Page { Main, Wifi };
    enum Item { ItemWifi, ItemFlip, ItemFont, ItemTz, ItemDim, ItemSleep, ItemAbout };

    static std::string tzText() {
        int m = storage::getInt("tz_min", 0);
        char buf[24];
        snprintf(buf, sizeof buf, "UTC%c%d:%02d", m < 0 ? '-' : '+', abs(m) / 60, abs(m) % 60);
        return buf;
    }

    void draw() {
        os::Gfx& g = os::gfx();
        if (page_ == Page::Main) {
            os::Rect r = ui::footer(g, os::content(), "Enter: change");
            rect_ = ui::header(g, r, "Settings");
            main_.items.clear();
            main_.items.push_back({"Wi-Fi", net::connected() ? net::ssid() : "not connected", 0});
            main_.items.push_back({"Rotate screen 180", storage::getInt("flip", 0) ? "on" : "off", 0});
            main_.items.push_back({"Terminal font", os::TermView::fontName(os::TermView::savedFont()), 0});
            main_.items.push_back({"Time zone", tzText(), 0});
            main_.items.push_back({"Dim screen on battery", minutesText(power::dimMinutes()), 0});
            main_.items.push_back({"Sleep on battery", minutesText(power::sleepMinutes()), 0});
            main_.items.push_back({"About", "", 0});
            main_.draw(g, rect_);
        } else {
            os::Rect r = ui::footer(g, os::content(), "Enter: connect   f: forget   r: rescan   Backspace: back");
            rect_ = ui::header(g, r, "Wi-Fi networks");
            wifi_.emptyText = "No networks found (r: rescan)";
            wifi_.draw(g, rect_);
        }
    }

    void mainKey(const key::Event& ev) {
        os::Gfx& g = os::gfx();
        if (main_.onKey(g, rect_, ev) != ui::List::Activated) return;
        switch (main_.selected) {
            case ItemWifi:
                page_ = Page::Wifi;
                scan();
                break;
            case ItemFlip:
                storage::putInt("flip", storage::getInt("flip", 0) ? 0 : 1);
                os::displaySettingsChanged();  // repaints us
                return;
            case ItemFont:
            {
                std::vector<std::string> names;
                for (int i = 0; i < os::TermView::kFontCount; i++) names.push_back(os::TermView::fontName((os::TermView::Font)i));
                int pick = ui::choose("Terminal font", names, os::TermView::savedFont());
                if (pick >= 0) os::TermView::saveFont((os::TermView::Font)pick);
                break;
            }
            case ItemTz: {
                std::string v;
                if (ui::prompt("Time zone", v, false, "UTC offset in hours, e.g. -5 or 1 or 5.5")) {
                    char* end = nullptr;
                    double h = strtod(v.c_str(), &end);
                    if (end != v.c_str() && h >= -12 && h <= 14) {
                        storage::putInt("tz_min", (int)lround(h * 60));
                        net::applyTimezone();
                    } else {
                        ui::alert("Time zone", "That is not a valid UTC offset.");
                    }
                }
                break;
            }
            case ItemDim: {
                const int v = chooseMinutes("Dim screen after (on battery)", {0, 1, 2, 5, 10}, power::dimMinutes());
                if (v >= 0) power::setDimMinutes(v);
                break;
            }
            case ItemSleep: {
                const int v = chooseMinutes("Sleep after (on battery)", {0, 2, 5, 10, 15, 30}, power::sleepMinutes());
                if (v >= 0) power::setSleepMinutes(v);
                break;
            }
            case ItemAbout: {
                char buf[320];
                snprintf(buf, sizeof buf, "cardputerOS %s\nIP: %s\nMAC: %s\nFree memory: %u KB\nStorage: %s\nLast boots: %s",
                         os::kVersion, net::connected() ? net::ip().c_str() : "-", WiFi.macAddress().c_str(),
                         (unsigned)(ESP.getFreeHeap() / 1024), storage::mounted() ? "ok" : "not mounted",
                         os::bootHistory().c_str());
                ui::alert("About", buf);
                break;
            }
        }
        draw();
    }

    static std::string minutesText(int m) { return m <= 0 ? "off" : "after " + std::to_string(m) + " min"; }

    // Returns the chosen number of minutes (0 = off) or -1 if cancelled.
    static int chooseMinutes(const char* title, const std::vector<int>& options, int current) {
        std::vector<std::string> names;
        int initial = 0;
        for (size_t i = 0; i < options.size(); i++) {
            names.push_back(options[i] == 0 ? "Off" : std::to_string(options[i]) + (options[i] == 1 ? " minute" : " minutes"));
            if (options[i] == current) initial = (int)i;
        }
        const int pick = ui::choose(title, names, initial);
        return pick < 0 ? -1 : options[pick];
    }

    void scan() {
        os::Gfx& g = os::gfx();
        os::Rect c = os::content();
        g.fillRect(c.x, c.y, c.w, c.h, theme::bg);
        ui::busy("Scanning for Wi-Fi networks...");
        nets_ = net::scan();
        wifi_.items.clear();
        for (auto& n : nets_) {
            std::string detail;
            if (net::connected() && net::ssid() == n.ssid) detail = "connected  ";
            else if (n.saved) detail = "saved  ";
            if (!n.secured) detail += "open  ";
            detail += std::to_string(n.rssi) + " dBm";
            wifi_.items.push_back({n.ssid, detail, 0});
        }
        wifi_.selected = 0;
    }

    void wifiKey(const key::Event& ev) {
        os::Gfx& g = os::gfx();
        if (ev.code == key::Backspace || ev.code == key::Esc) {
            page_ = Page::Main;
            draw();
            return;
        }
        if (ev.is('r')) {
            scan();
            draw();
            return;
        }
        if (nets_.empty()) return;
        const net::Network& n = nets_[wifi_.selected];
        if (ev.is('f')) {
            if (n.saved && ui::confirm("Forget network", "Forget \"" + n.ssid + "\"?")) {
                net::forget(n.ssid);
                scan();
            }
            draw();
            return;
        }
        if (wifi_.onKey(g, rect_, ev) != ui::List::Activated) return;

        std::string pass;
        bool havePass = !n.secured || net::savedPassword(n.ssid, pass);
        if (!havePass && !ui::prompt(("Password for " + n.ssid).c_str(), pass, true)) {
            draw();
            return;
        }
        std::string ssid = n.ssid;  // scan() below invalidates `n`
        ui::busy("Connecting to " + ssid + "...");
        if (net::connect(ssid, pass, 20000, os::idle)) {
            page_ = Page::Main;
        } else {
            ui::alert("Wi-Fi", "Could not connect to \"" + ssid + "\". Check the password and try again.");
            if (net::isSaved(ssid) && ui::confirm("Wi-Fi", "Forget the saved password for \"" + ssid + "\"?")) {
                net::forget(ssid);
                scan();
            }
        }
        os::refreshStatus();
        draw();
    }

    Page page_ = Page::Main;
    ui::List main_, wifi_;
    os::Rect rect_{0, 0, 0, 0};
    std::vector<net::Network> nets_;
};

OS_REGISTER_APP(SettingsApp, "Settings", "Wi-Fi, display, time", theme::dracula::cyan, icons::gear, 90);

}  // namespace
