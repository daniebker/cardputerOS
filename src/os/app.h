// App framework. To add an app: create a .cpp in src/apps, derive from os::App
// and register it with OS_REGISTER_APP (or OS_REGISTER_APP_IN to put it in a
// home screen folder). It then shows up in the launcher.
#pragma once
#include <M5GFX.h>

#include "key.h"

namespace os {

// Base drawing class: the external display, or an off-screen canvas in debug builds.
using Gfx = lgfx::LovyanGFX;

struct Rect {
    int x, y, w, h;
};

class App {
public:
    virtual ~App() {}
    // Called once after the app object is created.
    virtual void onStart() {}
    // The app became the foreground app: the content area was cleared, repaint everything.
    virtual void onResume() {}
    // The app is going to the background (it keeps running, see onBackgroundTick).
    virtual void onPause() {}
    virtual void onKey(const key::Event&) {}
    // Called continuously while in the foreground / background. Must not block.
    virtual void onTick() {}
    virtual void onBackgroundTick() {}
    // Return true while the device must not go to sleep (e.g. an open connection).
    virtual bool keepAwake() { return false; }
    // Optional one-line notice shown on the status screen even while the app is in
    // the background (e.g. "2 new messages").
    virtual const char* notice() { return nullptr; }
    // Optional short text shown next to the app name in the status bar.
    virtual const char* statusText() { return nullptr; }

    // Ask the OS to close this app.
    void finish() { finished_ = true; }
    bool finished() const { return finished_; }

private:
    bool finished_ = false;
};

struct AppInfo {
    const char* name;
    const char* description;
    uint32_t color;  // 0xRRGGBB, home screen icon colour
    App* (*create)();
    // Home screen icon (see os/icons.h); nullptr = the first letter of the name.
    void (*icon)(Gfx& g, int x, int y, int size, uint32_t fg, uint32_t bg);
    int order;  // position on the home screen, lowest first
    // Home screen folder the app is shown in (e.g. "Games"); nullptr = the home screen itself.
    const char* folder;
};

struct AppRegistrar {
    explicit AppRegistrar(const AppInfo& info);
};

#define OS_REGISTER_APP(Class, name, description, color, icon, order) \
    static os::AppRegistrar s_registrar_##Class( \
        {name, description, color, []() -> os::App* { return new Class(); }, icon, order, nullptr})

// Same, but the app is placed in a folder on the home screen. Apps with the same
// folder name are grouped; the folder sits where its first app would be.
#define OS_REGISTER_APP_IN(Class, folder, name, description, color, icon, order) \
    static os::AppRegistrar s_registrar_##Class( \
        {name, description, color, []() -> os::App* { return new Class(); }, icon, order, folder})

}  // namespace os
