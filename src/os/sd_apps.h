// Lua apps on the SD card. Each app is a folder /cardputerOS/apps/<id>/ with a
// main.lua (and optionally app.ini and more files). They appear on the home
// screen like built-in apps; the code is read from the card when the app starts.
// See docs/lua-apps.md.
#pragma once
#include <string>
#include <vector>

namespace sdapps {

constexpr const char* kRoot = "/cardputerOS/apps";

struct ScanResult {
    bool card = false;                  // a card was found
    std::vector<std::string> apps;      // names of the apps found
    std::vector<std::string> problems;  // folders that are not usable apps, and why
};

// Looks for apps on the card and updates the home screen. Apps that are running
// stay as they are; apps no longer on the card disappear (when not running).
ScanResult scan();

}  // namespace sdapps
