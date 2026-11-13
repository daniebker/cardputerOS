#include "wardrive.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace wardrive {

const char* sortName(Sort s) {
    switch (s) {
        case Sort::Signal: return "signal";
        case Sort::Newest: return "newest";
        case Sort::Name: return "name";
    }
    return "";
}

// ---------------------------------------------------------------- Table

int Table::find(Kind kind, const Mac& mac) const {
    for (size_t i = 0; i < devices_.size(); i++)
        if (devices_[i].kind == kind && devices_[i].mac == mac) return (int)i;
    return -1;
}

int Table::observe(const Device& seen, uint32_t nowMs, int64_t nowTime, bool* isNew) {
    int i = find(seen.kind, seen.mac);
    if (isNew) *isNew = i < 0;
    if (i < 0) {
        Device d = seen;
        d.best = seen.rssi;
        d.firstMs = d.lastMs = nowMs;
        d.firstTime = nowTime;
        d.sightings = 1;
        total_[(int)seen.kind]++;
        if ((size_t)count_[(int)seen.kind] < capacity_) {
            devices_.push_back(d);
            count_[(int)seen.kind]++;
            return (int)devices_.size() - 1;
        }
        // full: replace the device of this kind not seen for the longest time
        int oldest = -1;
        for (size_t k = 0; k < devices_.size(); k++) {
            if (devices_[k].kind != seen.kind) continue;
            if (oldest < 0 || (uint32_t)(nowMs - devices_[k].lastMs) > (uint32_t)(nowMs - devices_[oldest].lastMs))
                oldest = (int)k;
        }
        devices_[oldest] = d;
        return oldest;
    }
    Device& d = devices_[i];
    d.rssi = seen.rssi;
    if (seen.rssi > d.best) d.best = seen.rssi;
    d.lastMs = nowMs;
    d.sightings++;
    if (!seen.name.empty()) d.name = seen.name;
    if (seen.kind == Kind::Wifi) {
        d.channel = seen.channel;
        d.auth = seen.auth;
    } else {
        d.addrType = seen.addrType;
        if (seen.company != kNoCompany) d.company = seen.company;
    }
    if (d.firstTime == 0) d.firstTime = nowTime;  // clock became valid meanwhile
    return i;
}

int Table::active(Kind kind, uint32_t nowMs, uint32_t windowMs) const {
    int n = 0;
    for (auto& d : devices_)
        if (d.kind == kind && (uint32_t)(nowMs - d.lastMs) <= windowMs) n++;
    return n;
}

namespace {
std::string lower(const std::string& s) {
    std::string out(s);
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}
}  // namespace

std::vector<int> Table::sorted(Kind kind, Sort sort, uint32_t nowMs, uint32_t windowMs) const {
    std::vector<int> idx;
    for (size_t i = 0; i < devices_.size(); i++)
        if (devices_[i].kind == kind) idx.push_back((int)i);
    auto age = [&](int i) { return (uint32_t)(nowMs - devices_[i].lastMs); };
    switch (sort) {
        case Sort::Signal:
            std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
                const bool aa = age(a) <= windowMs, ab = age(b) <= windowMs;
                if (aa != ab) return aa;
                if (aa && devices_[a].rssi != devices_[b].rssi) return devices_[a].rssi > devices_[b].rssi;
                if (age(a) != age(b)) return age(a) < age(b);
                return devices_[a].mac < devices_[b].mac;
            });
            break;
        case Sort::Newest:
            std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
                const uint32_t fa = nowMs - devices_[a].firstMs, fb = nowMs - devices_[b].firstMs;
                if (fa != fb) return fa < fb;
                return devices_[a].mac < devices_[b].mac;
            });
            break;
        case Sort::Name: {
            std::vector<std::string> keys(devices_.size());
            for (int i : idx) keys[i] = lower(displayName(devices_[i]));
            std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
                // named devices first
                const bool na = !devices_[a].name.empty(), nb = !devices_[b].name.empty();
                if (na != nb) return na;
                if (keys[a] != keys[b]) return keys[a] < keys[b];
                return devices_[a].mac < devices_[b].mac;
            });
            break;
        }
    }
    return idx;
}

void Table::clear() {
    devices_.clear();
    count_[0] = count_[1] = 0;
    total_[0] = total_[1] = 0;
}

// ---------------------------------------------------------------- names

std::string macString(const Mac& mac) {
    char buf[18];
    snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

const char* authName(uint8_t auth) {
    switch (auth) {
        case AuthOpen: return "open";
        case AuthWep: return "WEP";
        case AuthWpa: return "WPA";
        case AuthWpa2: return "WPA2";
        case AuthWpaWpa2: return "WPA/WPA2";
        case AuthWpa2Enterprise: return "WPA2-EAP";
        case AuthWpa3: return "WPA3";
        case AuthWpa2Wpa3: return "WPA2/WPA3";
        case AuthWapi: return "WAPI";
        case AuthOwe: return "OWE";
    }
    return "?";
}

namespace {
// The WiGLE "AuthMode" column (Android capability string style).
const char* wigleAuth(uint8_t auth) {
    switch (auth) {
        case AuthOpen: return "[ESS]";
        case AuthWep: return "[WEP][ESS]";
        case AuthWpa: return "[WPA-PSK][ESS]";
        case AuthWpa2: return "[WPA2-PSK][ESS]";
        case AuthWpaWpa2: return "[WPA-PSK][WPA2-PSK][ESS]";
        case AuthWpa2Enterprise: return "[WPA2-EAP][ESS]";
        case AuthWpa3: return "[WPA3-SAE][ESS]";
        case AuthWpa2Wpa3: return "[WPA2-PSK][WPA3-SAE][ESS]";
        case AuthWapi: return "[WAPI-PSK][ESS]";
        case AuthOwe: return "[OWE][ESS]";
    }
    return "[ESS]";
}
}  // namespace

const char* companyName(uint16_t id) {
    // Bluetooth SIG company identifiers commonly seen in advertisements.
    switch (id) {
        case 0x0006: return "Microsoft";
        case 0x004C: return "Apple";
        case 0x0059: return "Nordic";
        case 0x0075: return "Samsung";
        case 0x0087: return "Garmin";
        case 0x009E: return "Bose";
        case 0x00E0: return "Google";
        case 0x012D: return "Sony";
        case 0x0171: return "Amazon";
        case 0x027D: return "Huawei";
        case 0x02E5: return "Espressif";
        case 0x038F: return "Xiaomi";
        case 0x0499: return "Ruuvi";
    }
    return nullptr;
}

std::string displayName(const Device& d) {
    if (!d.name.empty()) return d.name;
    if (d.kind == Kind::Wifi) return "(hidden)";
    if (const char* c = companyName(d.company)) return std::string(c) + " device";
    return "unnamed";
}

std::string ago(uint32_t ms) {
    const uint32_t s = ms / 1000;
    char buf[16];
    if (s < 5) return "now";
    if (s < 60) snprintf(buf, sizeof buf, "%us", (unsigned)s);
    else if (s < 3600) snprintf(buf, sizeof buf, "%um", (unsigned)(s / 60));
    else if (s < 86400) snprintf(buf, sizeof buf, "%uh", (unsigned)(s / 3600));
    else snprintf(buf, sizeof buf, "%ud", (unsigned)(s / 86400));
    return buf;
}

std::string formatTime(int64_t unixTime) {
    if (unixTime <= 0) return "";
    const time_t t = (time_t)unixTime;
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[80];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

// ---------------------------------------------------------------- CSV

std::string csvField(const std::string& s) {
    std::string clean;
    bool quote = false;
    for (char c : s) {
        if ((unsigned char)c < 0x20 || c == 0x7F) c = '?';
        if (c == ',' || c == '"') quote = true;
        clean.push_back(c);
        if (c == '"') clean.push_back('"');
    }
    if (!clean.empty() && (clean.front() == ' ' || clean.back() == ' ')) quote = true;
    return quote ? "\"" + clean + "\"" : clean;
}

std::string csvHeader(const std::string& appVersion) {
    return "WigleWifi-1.4,appRelease=" + appVersion + ",model=Cardputer ADV,release=" + appVersion +
           ",device=cardputerOS,display=ILI9341,board=ESP32-S3,brand=M5Stack\n"
           "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,"
           "AccuracyMeters,Type\n";
}

std::string csvRow(const Device& d) {
    std::string row = macString(d.mac) + ",";
    row += csvField(d.name) + ",";
    if (d.kind == Kind::Wifi) {
        row += wigleAuth(d.auth);
    } else {
        row += "Misc [LE]";
        if (const char* c = companyName(d.company)) row += std::string(" ") + c;
    }
    row += "," + formatTime(d.firstTime) + ",";
    row += std::to_string(d.kind == Kind::Wifi ? d.channel : 0) + ",";
    row += std::to_string(d.rssi) + ",0.0,0.0,0,0,";
    row += d.kind == Kind::Wifi ? "WIFI" : "BLE";
    row += "\n";
    return row;
}

}  // namespace wardrive
