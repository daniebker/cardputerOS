// The OS core: app lifecycle, launcher, status bar, main loop.
#pragma once
#include <string>

#include "app.h"

namespace os {

constexpr const char* kVersion = "0.3.0";

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

}  // namespace os
