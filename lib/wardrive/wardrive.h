// Wardriving: a table of the Wi-Fi networks and Bluetooth LE devices seen during
// a session, plus the CSV log format (WiGLE CSV 1.4, without location: the
// latitude / longitude columns are 0 because the device has no GPS).
//
// Portable, no platform dependencies.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wardrive {

enum class Kind : uint8_t { Wifi, Ble };

using Mac = std::array<uint8_t, 6>;  // as written, most significant byte first

constexpr uint16_t kNoCompany = 0xFFFF;

// Wi-Fi security, same numbering as ESP-IDF's wifi_auth_mode_t.
enum Auth : uint8_t {
    AuthOpen = 0,
    AuthWep,
    AuthWpa,
    AuthWpa2,
    AuthWpaWpa2,
    AuthWpa2Enterprise,
    AuthWpa3,
    AuthWpa2Wpa3,
    AuthWapi,
    AuthOwe,
};

struct Device {
    Kind kind = Kind::Wifi;
    Mac mac{};
    std::string name;      // SSID or advertised name; "" = hidden / not advertised
    int8_t rssi = 0;       // signal at the last sighting (dBm)
    int8_t best = -127;    // strongest signal seen
    uint8_t channel = 0;   // Wi-Fi channel
    uint8_t auth = 0;      // Wi-Fi: Auth
    uint8_t addrType = 0;  // BLE: 0 public, 1 random
    uint16_t company = kNoCompany;  // BLE: manufacturer ID from the advertisement
    uint32_t firstMs = 0, lastMs = 0;  // uptime of the first / last sighting
    int64_t firstTime = 0;  // wall clock of the first sighting (Unix time), 0 = clock not set
    uint32_t sightings = 0;
};

enum class Sort : uint8_t { Signal, Newest, Name };
const char* sortName(Sort s);

class Table {
public:
    // At most `capacity` devices of each kind are kept; when full, the one not
    // seen for the longest time is dropped.
    explicit Table(size_t capacity = 400) : capacity_(capacity) {}

    // Records a sighting. `seen` carries kind, mac, rssi and whatever else the radio
    // reported. Returns the device's index; *isNew is set if it was not in the table.
    int observe(const Device& seen, uint32_t nowMs, int64_t nowTime, bool* isNew = nullptr);

    int find(Kind kind, const Mac& mac) const;
    const Device& at(int index) const { return devices_[index]; }
    const std::vector<Device>& devices() const { return devices_; }

    int count(Kind kind) const { return count_[(int)kind]; }
    // Devices first seen this session, including ones dropped from the table since.
    uint32_t total(Kind kind) const { return total_[(int)kind]; }
    // Devices seen within the last `windowMs`.
    int active(Kind kind, uint32_t nowMs, uint32_t windowMs) const;

    // Indices of the devices of `kind` in display order. Signal: devices seen within
    // `windowMs` strongest first, then the others most recently seen first.
    std::vector<int> sorted(Kind kind, Sort sort, uint32_t nowMs, uint32_t windowMs) const;

    void clear();

private:
    size_t capacity_;
    std::vector<Device> devices_;
    int count_[2] = {0, 0};
    uint32_t total_[2] = {0, 0};
};

std::string macString(const Mac& mac);  // "aa:bb:cc:dd:ee:ff"
const char* authName(uint8_t auth);      // short, e.g. "WPA2"
const char* companyName(uint16_t id);    // well-known manufacturers, nullptr otherwise
// Name for lists: the SSID / device name, else "(hidden)", the manufacturer, or "unnamed".
std::string displayName(const Device& d);
// "now", "42s", "5m", "3h", "2d"
std::string ago(uint32_t ms);
// "2026-11-13 14:05:09" (UTC), "" for 0
std::string formatTime(int64_t unixTime);

// --- CSV log (WiGLE 1.4) ---
std::string csvHeader(const std::string& appVersion);
std::string csvRow(const Device& d);
// Quotes a CSV field when needed; control characters become '?'.
std::string csvField(const std::string& s);

}  // namespace wardrive
