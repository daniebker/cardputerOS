// Cardputer keyboard -> key::Event translation (with key repeat and the Fn layer).
#pragma once
#include "key.h"

namespace hal {

void keyboardBegin();
// Returns true and fills `ev` when a key event is available. Call repeatedly.
bool keyboardPoll(key::Event& ev);

// Around light sleep: empties the keyboard chip's event queue so its interrupt line
// is released (it is the wake-up signal), and restores the driver afterwards. The
// key press that woke the device is discarded.
void keyboardBeforeSleep();
void keyboardAfterSleep();

#ifdef SCREENSHOT_DEBUG
// Debug build: keys can also be typed over USB serial; 0x1D requests a screenshot.
bool takeScreenshotRequest();
// 0x1A over serial: sleep for a few seconds (tests the sleep / wake path).
bool takeSleepTestRequest();
#endif

}  // namespace hal
