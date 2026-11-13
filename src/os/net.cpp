#include "net.h"

#include <WiFi.h>
#include <time.h>

#include <algorithm>

#include "storage.h"

namespace net {

namespace {
const char* kKey = "wifi";  // NVS: one "ssid<TAB>password" line per network
bool manualConnect = false;
struct Saved {
    std::string ssid, pass;
};
std::vector<Saved> saved;
bool scanning = false;
bool userDisconnected = false;
bool ntpStarted = false;
uint32_t nextAttempt = 0;
std::string target;

void load() {
    saved.clear();
    std::string text = storage::getStr(kKey);
    for (auto& line : storage::splitLines(text)) {
        auto f = storage::splitFields(line);
        if (f.empty() || f[0].empty()) continue;
        saved.push_back({f[0], f.size() > 1 ? f[1] : ""});
    }
}

void save() {
    std::string text;
    for (auto& s : saved) text += s.ssid + "\t" + s.pass + "\n";
    storage::putStr(kKey, text);
}

const Saved* findSaved(const std::string& ssid) {
    for (auto& s : saved)
        if (s.ssid == ssid) return &s;
    return nullptr;
}

// Sets the local time zone from the "tz_min" setting (minutes east of UTC).
// POSIX TZ strings count the other way: UTC+1 is "UTC-1".
void setLocalZone() {
    const int m = storage::getInt("tz_min", 0);
    char tz[24];
    snprintf(tz, sizeof tz, "UTC%c%d:%02d", m > 0 ? '-' : '+', abs(m) / 60, abs(m) % 60);
    setenv("TZ", tz, 1);
    tzset();
}

void startNtp() {
    if (ntpStarted) return;
    ntpStarted = true;
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
    setLocalZone();  // configTime resets TZ
}
}  // namespace

namespace {
bool begun = false;
}

bool started() { return begun; }

void begin() {
    begun = true;
    load();
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setHostname("cardputer");
    nextAttempt = millis() + 500;
}

void applyTimezone() {
    setLocalZone();
    ntpStarted = false;
    if (connected()) startNtp();
}

void tick() {
    if (!begun) return;
    if (connected()) {
        startNtp();
        return;
    }
    if (saved.empty() || userDisconnected || manualConnect) return;

    if (scanning) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) return;
        scanning = false;
        const Saved* best = nullptr;
        int bestRssi = -1000;
        for (int i = 0; i < n; i++) {
            const Saved* s = findSaved(WiFi.SSID(i).c_str());
            if (s && WiFi.RSSI(i) > bestRssi) {
                best = s;
                bestRssi = WiFi.RSSI(i);
            }
        }
        WiFi.scanDelete();
        if (best) {
            target = best->ssid;
            WiFi.begin(best->ssid.c_str(), best->pass.empty() ? nullptr : best->pass.c_str());
            nextAttempt = millis() + 20000;
        } else {
            nextAttempt = millis() + 30000;
        }
        return;
    }
    if ((int32_t)(millis() - nextAttempt) >= 0) {
        target.clear();
        WiFi.disconnect();
        if (WiFi.scanNetworks(true) == WIFI_SCAN_FAILED) {
            nextAttempt = millis() + 5000;
        } else {
            scanning = true;
        }
    }
}

bool connected() { return WiFi.status() == WL_CONNECTED; }
std::string ssid() { return connected() ? std::string(WiFi.SSID().c_str()) : target; }
std::string ip() { return connected() ? std::string(WiFi.localIP().toString().c_str()) : std::string(); }
int rssi() { return connected() ? WiFi.RSSI() : 0; }

std::vector<Network> scan() {
    // let a background scan finish first
    uint32_t t0 = millis();
    while (scanning && WiFi.scanComplete() == WIFI_SCAN_RUNNING && millis() - t0 < 8000) delay(50);
    scanning = false;
    WiFi.scanDelete();

    std::vector<Network> out;
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
        Network nw;
        nw.ssid = WiFi.SSID(i).c_str();
        if (nw.ssid.empty()) continue;
        nw.rssi = WiFi.RSSI(i);
        nw.secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        nw.saved = isSaved(nw.ssid);
        auto it = std::find_if(out.begin(), out.end(), [&](const Network& o) { return o.ssid == nw.ssid; });
        if (it == out.end()) out.push_back(nw);
        else if (nw.rssi > it->rssi) *it = nw;
    }
    WiFi.scanDelete();
    std::sort(out.begin(), out.end(), [](const Network& a, const Network& b) { return a.rssi > b.rssi; });
    nextAttempt = millis() + 3000;
    return out;
}

bool connect(const std::string& ssidName, const std::string& pass, uint32_t timeoutMs, void (*idle)()) {
    scanning = false;
    userDisconnected = false;
    manualConnect = true;  // keep the auto-connect logic out of the way
    WiFi.scanDelete();
    WiFi.disconnect();
    delay(100);
    target = ssidName;
    WiFi.begin(ssidName.c_str(), pass.empty() ? nullptr : pass.c_str());
    uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED) break;
        if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
            // the driver reports these transiently; give it a little time before believing them
            if (millis() - t0 > 6000) break;
        }
        if (idle) idle();
        delay(20);
    }
    manualConnect = false;
    if (!connected()) {
        WiFi.disconnect();
        target.clear();
        nextAttempt = millis() + 5000;
        return false;
    }
    bool found = false;
    for (auto& s : saved) {
        if (s.ssid == ssidName) {
            s.pass = pass;
            found = true;
        }
    }
    if (!found) saved.push_back({ssidName, pass});
    save();
    return true;
}

bool isSaved(const std::string& name) { return findSaved(name) != nullptr; }

bool savedPassword(const std::string& name, std::string& pass) {
    const Saved* s = findSaved(name);
    if (!s) return false;
    pass = s->pass;
    return true;
}

void forget(const std::string& name) {
    saved.erase(std::remove_if(saved.begin(), saved.end(), [&](const Saved& s) { return s.ssid == name; }),
                saved.end());
    save();
    if (connected() && name == WiFi.SSID().c_str()) {
        WiFi.disconnect();
        target.clear();
    }
}

void suspend() {
    if (!begun) return;
    scanning = false;
    target.clear();
    WiFi.scanDelete();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

void resume() {
    if (!begun) return;
    WiFi.mode(WIFI_STA);
    ntpStarted = false;
    nextAttempt = millis() + 500;
}

void disconnect() {
    userDisconnected = true;
    scanning = false;
    target.clear();
    WiFi.disconnect();
}

}  // namespace net
