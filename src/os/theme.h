// UI colours (0xRRGGBB) and fonts. Colour scheme: Dracula (draculatheme.com).
#pragma once
#include <M5GFX.h>

namespace theme {

// Dracula palette
namespace dracula {
constexpr uint32_t background = 0x282A36;
constexpr uint32_t currentLine = 0x44475A;
constexpr uint32_t foreground = 0xF8F8F2;
constexpr uint32_t comment = 0x6272A4;
constexpr uint32_t cyan = 0x8BE9FD;
constexpr uint32_t green = 0x50FA7B;
constexpr uint32_t orange = 0xFFB86C;
constexpr uint32_t pink = 0xFF79C6;
constexpr uint32_t purple = 0xBD93F9;
constexpr uint32_t red = 0xFF5555;
constexpr uint32_t yellow = 0xF1FA8C;
constexpr uint32_t darker = 0x21222C;   // panels
constexpr uint32_t darkest = 0x191A21;
}  // namespace dracula

constexpr uint32_t bg = dracula::background;
constexpr uint32_t panel = dracula::darker;
constexpr uint32_t border = dracula::comment;
constexpr uint32_t text = dracula::foreground;
constexpr uint32_t dim = dracula::comment;
constexpr uint32_t accent = dracula::purple;
constexpr uint32_t select = dracula::currentLine;
constexpr uint32_t ok = dracula::green;
constexpr uint32_t warn = dracula::orange;
constexpr uint32_t error = dracula::red;

// Terminal (SSH)
constexpr uint32_t termBg = dracula::background;
constexpr uint32_t termFg = dracula::foreground;
constexpr uint32_t termCursor = dracula::foreground;

// Notes editor (Markdown)
constexpr uint32_t editorBg = dracula::background;
constexpr uint32_t editorSelect = dracula::currentLine;
constexpr uint32_t editorCursor = dracula::purple;
constexpr uint32_t mdHeading = dracula::purple;
constexpr uint32_t mdCode = dracula::green;
constexpr uint32_t mdQuote = dracula::yellow;
constexpr uint32_t mdMarker = dracula::pink;
constexpr uint32_t editorTilde = dracula::comment;

inline const lgfx::IFont* fontSmall() { return &fonts::DejaVu9; }
inline const lgfx::IFont* fontUi() { return &fonts::DejaVu12; }
inline const lgfx::IFont* fontBig() { return &fonts::DejaVu18; }

}  // namespace theme
