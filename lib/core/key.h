// Portable key event type shared by the keyboard driver, the OS and the apps.
#pragma once
#include <cstdint>

namespace key {

// Codes below 0x80 are plain ASCII (already shifted: 'a' vs 'A', '1' vs '!').
enum Code : uint16_t {
    None = 0,
    Backspace = 0x08,
    Tab = 0x09,
    Enter = 0x0D,
    Esc = 0x1B,
    Up = 0x100,
    Down,
    Left,
    Right,
    Home,
    End,
    PageUp,
    PageDown,
    Delete,
    F1,  // F1..F10 are contiguous
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    OsHome,  // "go to launcher" (G0 button or Opt+Esc)
};

enum Mod : uint8_t {
    Shift = 1,
    Ctrl = 2,
    Alt = 4,
    Opt = 8,  // reserved for OS / app shortcuts, never sent to a remote host
};

struct Event {
    uint16_t code = None;
    uint8_t mods = 0;
    bool repeat = false;

    bool ctrl() const { return mods & Ctrl; }
    bool alt() const { return mods & Alt; }
    bool opt() const { return mods & Opt; }
    bool shift() const { return mods & Shift; }
    bool printable() const { return code >= 0x20 && code < 0x7F; }
    // True for an unmodified (or shift-only) printable character.
    bool isChar() const { return printable() && !(mods & (Ctrl | Alt | Opt)); }
    bool is(uint16_t c) const { return code == c && !(mods & (Ctrl | Alt | Opt)); }
    bool isOpt(uint16_t c) const { return code == c && (mods & Opt); }
    bool isCtrl(uint16_t c) const { return code == c && (mods & Ctrl) && !(mods & (Alt | Opt)); }
};

}  // namespace key
