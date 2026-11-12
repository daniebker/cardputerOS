// Cardputer keyboard -> key::Event translation (with key repeat and the Fn layer).
#pragma once
#include "key.h"

namespace hal {

void keyboardBegin();
// Returns true and fills `ev` when a key event is available. Call repeatedly.
bool keyboardPoll(key::Event& ev);

#ifdef SCREENSHOT_DEBUG
// Debug build: keys can also be typed over USB serial; 0x1D requests a screenshot.
bool takeScreenshotRequest();
#endif

}  // namespace hal
