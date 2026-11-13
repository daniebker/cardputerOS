// The OS core: app lifecycle, launcher, status bar, main loop.
#pragma once
#include <string>

#include "app.h"

namespace os {

constexpr const char* kVersion = "0.9.0";

void begin();
void loopOnce();

Gfx& gfx();
// Screen area available to apps: the whole external display (status is shown
// on the Cardputer's built-in screen).
Rect content();

// For blocking dialogs: keeps the OS services running and fetches the next key.
// When the user asks for the launcher, this returns key::Esc until the dialog
// stack has unwound, then the OS switches to the launcher.
bool modalPoll(key::Event& ev);
// Keeps services running without reading keys (use inside long waits).
void idle();

// Updates the status on the built-in screen now (it also refreshes on its own).
void refreshStatus();
// Why the last few boots happened, newest first (e.g. "power-on, brownout, ...").
std::string bootHistory();
// Re-apply display settings (rotation) and repaint the foreground app.
void displaySettingsChanged();

// Apps added and removed at run time (the Lua apps on the SD card). `info.ctx`
// identifies the app and must be unique; the strings in `info` must stay valid
// until the app is removed. Removing a running app closes it first.
void addApp(const AppInfo& info);
void removeApp(void* ctx);
bool appRunning(void* ctx);
// True while the user asked for the home screen and the foreground app has not
// returned yet (dialogs are being closed).
bool homeRequested();

}  // namespace os
