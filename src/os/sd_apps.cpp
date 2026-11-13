// Lua apps on the SD card: finding them, and running one as an os::App on top of
// the portable script runtime (lib/script).
#include "sd_apps.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <time.h>

#include <algorithm>
#include <list>
#include <memory>

#include "../hal/sdcard.h"
#include "icons.h"
#include "kernel.h"
#include "net.h"
#include "script.h"
#include "storage.h"
#include "theme.h"
#include "ui.h"

namespace sdapps {

namespace {

constexpr int kOrder = 80;                // home screen position: after the built-in apps, before Settings
constexpr size_t kMaxSource = 128 * 1024;  // largest main.lua
constexpr size_t kReserve = 48 * 1024;     // heap left for the OS (Wi-Fi, TLS, ...) when sizing the Lua memory
constexpr size_t kMaxMemory = 160 * 1024;
constexpr size_t kMinMemory = 24 * 1024;

struct Entry {
    script::Manifest m;
};

// std::list: entries never move, so the strings the kernel points to stay valid.
std::list<Entry>& entries() {
    static std::list<Entry> e;
    return e;
}

std::string appDir(const std::string& id) { return std::string(kRoot) + "/" + id; }

std::string sdError(hal::SdResult r) {
    switch (r) {
        case hal::SdResult::NoCard: return "no SD card";
        case hal::SdResult::NotFound: return "not found";
        case hal::SdResult::Error: return "SD card error";
        default: return "";
    }
}

const lgfx::IFont* fontFor(script::Font f) {
    switch (f) {
        case script::Font::Small: return theme::fontSmall();
        case script::Font::Large: return theme::fontBig();
        case script::Font::Mono: return &fonts::Font0;
        case script::Font::Bold: return &fonts::FreeSansBold9pt7b;
        default: return theme::fontUi();
    }
}

// ------------------------------------------------------------ the device, as seen by a script
class DeviceHost : public script::Host {
public:
    explicit DeviceHost(const std::string& id) : id_(id), dir_(appDir(id)) {}
    bool active = false;  // the app is in the foreground and may draw

    uint32_t millis() override { return ::millis(); }
    uint32_t random32() override { return esp_random(); }
    void log(const std::string& line) override { Serial.printf("[%s] %s\n", id_.c_str(), line.c_str()); }
    bool interrupted() override { return os::homeRequested(); }

    bool canDraw() override { return active; }
    int width() override { return os::content().w; }
    int height() override { return os::content().h; }
    void fillRect(int x, int y, int w, int h, uint32_t c) override { os::gfx().fillRect(x, y, w, h, c); }
    void drawRect(int x, int y, int w, int h, uint32_t c) override { os::gfx().drawRect(x, y, w, h, c); }
    void fillRoundRect(int x, int y, int w, int h, int r, uint32_t c) override {
        os::gfx().fillRoundRect(x, y, w, h, r, c);
    }
    void drawRoundRect(int x, int y, int w, int h, int r, uint32_t c) override {
        os::gfx().drawRoundRect(x, y, w, h, r, c);
    }
    void drawLine(int x0, int y0, int x1, int y1, uint32_t c) override { os::gfx().drawLine(x0, y0, x1, y1, c); }
    void drawPixel(int x, int y, uint32_t c) override { os::gfx().drawPixel(x, y, c); }
    void drawCircle(int x, int y, int r, uint32_t c) override { os::gfx().drawCircle(x, y, r, c); }
    void fillCircle(int x, int y, int r, uint32_t c) override { os::gfx().fillCircle(x, y, r, c); }
    void drawTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c) override {
        os::gfx().drawTriangle(x0, y0, x1, y1, x2, y2, c);
    }
    void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c) override {
        os::gfx().fillTriangle(x0, y0, x1, y1, x2, y2, c);
    }

    void drawText(const std::string& text, int x, int y, script::Font f, int scale, script::Align a, uint32_t fg,
                  bool hasBg, uint32_t bg) override {
        os::Gfx& g = os::gfx();
        const std::string s = ui::displayable(text);
        g.setFont(fontFor(f));
        g.setTextSize(scale);
        const int w = g.textWidth(s.c_str());  // both measured with the scale
        const int left = a == script::Align::Center ? x - w / 2 : (a == script::Align::Right ? x - w : x);
        g.startWrite();
        if (hasBg) g.fillRect(left, y, w, g.fontHeight(), bg);
        g.setTextDatum(textdatum_t::top_left);
        g.setTextColor(fg);
        g.drawString(s.c_str(), left, y);
        g.endWrite();
        g.setTextSize(1);  // everything else draws unscaled
    }
    int textWidth(const std::string& text, script::Font f) override {
        os::Gfx& g = os::gfx();
        g.setFont(fontFor(f));
        return g.textWidth(ui::displayable(text).c_str());
    }
    int fontHeight(script::Font f) override {
        os::Gfx& g = os::gfx();
        g.setFont(fontFor(f));
        return g.fontHeight();
    }
    int header(const std::string& title) override {
        const os::Rect r = ui::header(os::gfx(), os::content(), ui::displayable(title).c_str());
        return r.y;
    }
    int footer(const std::string& hint) override {
        const os::Rect r = ui::footer(os::gfx(), os::content(), ui::displayable(hint).c_str());
        return r.y + r.h;
    }

    void alert(const std::string& title, const std::string& message) override {
        ui::alert(ui::displayable(title).c_str(), ui::displayable(message));
    }
    bool confirm(const std::string& title, const std::string& message, bool defaultYes) override {
        return ui::confirm(ui::displayable(title).c_str(), ui::displayable(message), defaultYes);
    }
    bool prompt(const std::string& title, std::string& value, bool secret) override {
        return ui::prompt(ui::displayable(title).c_str(), value, secret);
    }
    int choose(const std::string& title, const std::vector<std::string>& items, int initial) override {
        std::vector<std::string> shown;
        shown.reserve(items.size());
        for (auto& i : items) shown.push_back(ui::displayable(i));
        return ui::choose(ui::displayable(title).c_str(), shown, initial);
    }
    void busy(const std::string& message) override { ui::busy(ui::displayable(message)); }

    bool readFile(const std::string& path, std::string& out, size_t maxBytes, std::string& error) override {
        const hal::SdResult r = hal::sdReadFile(full(path).c_str(), out, maxBytes);
        if (r == hal::SdResult::Error) error = "could not read (larger than " + std::to_string(maxBytes / 1024) + " KB?)";
        else error = sdError(r);
        return r == hal::SdResult::Ok;
    }
    bool writeFile(const std::string& path, const std::string& data, bool append, std::string& error) override {
        const hal::SdResult r =
            append ? hal::sdAppendFile(full(path).c_str(), data) : hal::sdWriteFile(full(path).c_str(), data);
        error = sdError(r);
        return r == hal::SdResult::Ok;
    }
    bool listDir(const std::string& path, std::vector<script::FileEntry>& out, std::string& error) override {
        std::vector<hal::SdEntry> entries;
        const hal::SdResult r = hal::sdListDir(full(path).c_str(), entries);
        error = sdError(r);
        for (auto& e : entries) {
            script::FileEntry f;
            f.name = e.name;
            f.dir = e.dir;
            f.size = e.size;
            out.push_back(f);
        }
        return r == hal::SdResult::Ok;
    }
    bool removeFile(const std::string& path, std::string& error) override {
        const hal::SdResult r = hal::sdRemove(full(path).c_str());
        error = sdError(r);
        return r == hal::SdResult::Ok;
    }
    bool exists(const std::string& path) override { return hal::sdExists(full(path).c_str()); }

    // Save data lives in the internal flash, so it survives swapping SD cards.
    bool loadSave(std::string& out) override { return storage::readFile(savePath().c_str(), out); }
    bool storeSave(const std::string& data) override {
        storage::makeDir("/appdata");
        return storage::writeFile(savePath().c_str(), data);
    }

    int64_t unixTime() override {
        const time_t t = time(nullptr);
        return t > 1700000000 ? (int64_t)t : 0;
    }
    int battery() override { return M5.Power.getBatteryLevel(); }
    std::string wifi() override { return net::connected() ? net::ssid() : std::string(); }
    std::string osVersion() override { return os::kVersion; }

private:
    std::string full(const std::string& path) const { return path.empty() ? dir_ : dir_ + "/" + path; }
    std::string savePath() const { return "/appdata/" + id_ + ".lua"; }
    std::string id_, dir_;
};

// ------------------------------------------------------------ an SD card app
class ScriptApp : public os::App {
public:
    explicit ScriptApp(const script::Manifest& m) : manifest_(m), host_(m.id) {}

    ~ScriptApp() override {
        host_.active = false;
        if (rt_) rt_->stop();
        rt_.reset();
        Serial.printf("[%s] closed; loop stack left: %u bytes\n", manifest_.id.c_str(),
                      (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    }

    void onStart() override { load(); }

    void onResume() override {
        host_.active = true;
        if (!running()) {
            drawError();
            return;
        }
        rt_->draw();
        afterCall();
    }

    void onPause() override {
        if (running()) rt_->pause();
        host_.active = false;
    }

    void onKey(const key::Event& ev) override {
        if (!running()) {
            errorKey(ev);
            return;
        }
        rt_->key(ev);
        afterCall();
    }

    void onTick() override {
        if (!running()) return;
        rt_->tick(true);
        afterCall();
    }

    void onBackgroundTick() override {
        if (running()) rt_->tick(false);
    }

    bool keepAwake() override { return running() && rt_->keepAwake(); }

    const char* notice() override {
        if (rt_ && rt_->failed()) {
            notice_ = manifest_.name + " stopped with an error";
            return notice_.c_str();
        }
        return running() && !rt_->notice().empty() ? rt_->notice().c_str() : nullptr;
    }

    const char* statusText() override {
        if (!running()) return "stopped";
        return rt_->status().empty() ? nullptr : rt_->status().c_str();
    }

private:
    bool running() const { return rt_ && !rt_->failed(); }

    // Reads main.lua from the card and starts it.
    void load() {
        rt_.reset();
        loadError_.clear();
        host_.active = true;  // app.start may open dialogs
        ui::busy("Starting " + manifest_.name + "...");
        std::string src;
        const hal::SdResult r = hal::sdReadFile((appDir(manifest_.id) + "/main.lua").c_str(), src, kMaxSource);
        if (r != hal::SdResult::Ok) {
            if (r == hal::SdResult::NoCard) loadError_ = "No SD card found. Insert the card with the app and press r.";
            else if (r == hal::SdResult::NotFound) loadError_ = "main.lua is no longer on the SD card.";
            else loadError_ = "Could not read main.lua (larger than 128 KB?).";
            return;
        }
        const size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        script::Limits limits;
        limits.memory = freeHeap > kReserve ? std::min(kMaxMemory, freeHeap - kReserve) : 0;
        if (limits.memory < kMinMemory) {
            loadError_ = "Not enough free memory (" + std::to_string(freeHeap / 1024) +
                         " KB). Close other apps on the home screen (x) and press r.";
            return;
        }
        Serial.printf("[%s] starting, %u KB for Lua\n", manifest_.id.c_str(), (unsigned)(limits.memory / 1024));
        rt_.reset(new script::Runtime(host_, manifest_, limits));
        rt_->start(src);
    }

    void afterCall() {
        if (!rt_) return;
        if (rt_->failed()) {
            if (host_.active) drawError();
        } else if (rt_->exitRequested()) {
            finish();
        }
    }

    const std::string& errorText() const { return rt_ ? rt_->error() : loadError_; }

    void drawError() {
        os::Gfx& g = os::gfx();
        const os::Rect c = os::content();
        g.startWrite();
        g.fillRect(c.x, c.y, c.w, c.h, theme::bg);
        g.endWrite();
        os::Rect r = ui::footer(g, c, "r: restart (reads the app again)   Enter: close");
        g.setFont(theme::fontUi());
        g.setTextDatum(textdatum_t::top_left);
        g.setTextColor(theme::error);
        ui::drawClipped(g, ui::displayable(manifest_.name) + (rt_ ? " stopped" : " could not start"), r.x + 8, r.y + 6,
                        r.w - 16);
        g.drawFastHLine(r.x + 6, r.y + 24, r.w - 12, theme::border);
        // the message and traceback, wrapped at the screen edge
        g.setFont(theme::fontSmall());
        g.setTextColor(theme::text);
        const int lineH = g.fontHeight() + 1, maxW = r.w - 16;
        int y = r.y + 30;
        std::string text = ui::displayable(errorText());
        std::string line;
        auto flush = [&]() {
            if (y + lineH <= r.y + r.h) g.drawString(line.c_str(), r.x + 8, y);
            y += lineH;
            line.clear();
        };
        for (char ch : text) {
            if (ch == '\n') {
                flush();
                g.setTextColor(theme::dim);  // the traceback below the message
                continue;
            }
            if (ch == '\t') {
                line += "  ";
                continue;
            }
            if (g.textWidth((line + ch).c_str()) > maxW) flush();
            line += ch;
        }
        if (!line.empty()) flush();
    }

    void errorKey(const key::Event& ev) {
        if (ev.is(key::Enter) || ev.code == key::Esc) {
            finish();
        } else if (ev.is('r')) {
            load();
            os::Gfx& g = os::gfx();
            const os::Rect c = os::content();
            g.fillRect(c.x, c.y, c.w, c.h, theme::bg);
            onResume();
        }
    }

    script::Manifest manifest_;
    DeviceHost host_;
    std::unique_ptr<script::Runtime> rt_;
    std::string loadError_, notice_;
};

os::App* createApp(void* ctx) { return new ScriptApp(static_cast<Entry*>(ctx)->m); }

std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

}  // namespace

ScanResult scan() {
    ScanResult res;
    std::vector<script::Manifest> found;
    {
        hal::SdSession sd;  // one mount for the whole scan
        res.card = sd.ok();
        std::vector<hal::SdEntry> dirs;
        if (sd.ok() && hal::sdListDir(kRoot, dirs) == hal::SdResult::Ok) {
            for (const hal::SdEntry& d : dirs) {
                if (!d.dir) continue;
                if (d.name[0] == '.') continue;  // hidden folders (macOS adds some)
                if (!script::validId(d.name)) {
                    res.problems.push_back(d.name + ": use only letters, digits, - _ . in folder names");
                    continue;
                }
                const std::string dir = appDir(d.name);
                if (!hal::sdExists((dir + "/main.lua").c_str())) {
                    res.problems.push_back(d.name + ": no main.lua");
                    continue;
                }
                std::string ini, err;
                script::Manifest m;
                hal::sdReadFile((dir + "/app.ini").c_str(), ini, 4096);
                if (!script::parseManifest(d.name, ini, m, err)) {
                    res.problems.push_back(d.name + ": " + err);
                    script::parseManifest(d.name, "", m, err);  // start it with the defaults anyway
                }
                if (!m.icon.empty() && !icons::byName(m.icon.c_str()))
                    res.problems.push_back(d.name + ": unknown icon \"" + m.icon + "\"");
                found.push_back(m);
            }
        }
    }
    std::sort(found.begin(), found.end(), [](const script::Manifest& a, const script::Manifest& b) {
        return lower(a.name) < lower(b.name);
    });

    // Apps that are running stay as they are (even if they left the card); the
    // rest are registered anew, which also picks up changes to app.ini.
    auto& list = entries();
    std::vector<std::string> kept;
    for (auto it = list.begin(); it != list.end();) {
        if (os::appRunning(&*it)) {
            kept.push_back(it->m.id);
            ++it;
        } else {
            os::removeApp(&*it);
            it = list.erase(it);
        }
    }
    for (const script::Manifest& m : found) {
        res.apps.push_back(m.name);
        if (std::find(kept.begin(), kept.end(), m.id) != kept.end()) continue;
        list.push_back(Entry{m});
        Entry& e = list.back();
        os::AppInfo info{};
        info.name = e.m.name.c_str();
        info.description = e.m.description.c_str();
        info.color = e.m.color;
        info.create = createApp;
        info.icon = e.m.icon.empty() ? nullptr : icons::byName(e.m.icon.c_str());
        info.order = kOrder;
        info.folder = e.m.folder.empty() ? nullptr : e.m.folder.c_str();
        info.ctx = &e;
        os::addApp(info);
    }
    Serial.printf("SD apps: %u found%s\n", (unsigned)found.size(), res.card ? "" : " (no card)");
    for (auto& p : res.problems) Serial.printf("SD apps: %s\n", p.c_str());
    return res;
}

}  // namespace sdapps
