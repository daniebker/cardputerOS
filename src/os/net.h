// Wi-Fi management: saved networks, auto-connect, NTP.
#pragma once
#include <string>
#include <vector>

namespace net {

struct Network {
    std::string ssid;
    int rssi = 0;
    bool secured = false;
    bool saved = false;
};

void begin();
bool started();
void tick();  // call regularly from the main loop

bool connected();
std::string ssid();  // SSID we are connected (or connecting) to
std::string ip();
int rssi();

// Blocking scan (a few seconds). Strongest first, duplicates removed.
std::vector<Network> scan();

// Blocking connect (up to timeoutMs). `idle` is called while waiting.
// On success the credentials are saved for auto-connect.
bool connect(const std::string& ssid, const std::string& pass, uint32_t timeoutMs, void (*idle)() = nullptr);
bool isSaved(const std::string& ssid);
bool savedPassword(const std::string& ssid, std::string& pass);
void forget(const std::string& ssid);
void disconnect();

void applyTimezone();  // re-read the "tz_min" setting

}  // namespace net
