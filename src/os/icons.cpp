#include "icons.h"

#include <cmath>

#include "theme.h"

namespace icons {

namespace dr = theme::dracula;

void terminal(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t) {
    const int top = y + s / 8, h = s - s / 4;
    g.fillRoundRect(x, top, s, h, 6, fg);                         // frame
    g.fillRoundRect(x + 3, top + 11, s - 6, h - 14, 3, dr::darkest);  // screen
    for (int i = 0; i < 3; i++) g.fillCircle(x + 8 + i * 7, top + 6, 2, dr::darkest);  // title bar dots
    // prompt: ">" and a cursor
    const int px = x + 10, py = top + 11 + (h - 14) / 2;
    for (int t = 0; t < 3; t++) {
        g.drawLine(px, py - 7 + t, px + 8, py + t, fg);
        g.drawLine(px + 8, py + t, px, py + 7 + t, fg);
    }
    g.fillRect(px + 14, py + 5, 12, 3, dr::foreground);
}

void note(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t bg) {
    const int px = x + s / 7, w = s - 2 * (s / 7), fold = s / 4;
    g.fillRoundRect(px, y, w, s, 4, fg);
    // folded corner
    g.fillTriangle(px + w - fold, y, px + w, y, px + w, y + fold, bg);
    g.fillTriangle(px + w - fold, y, px + w - fold, y + fold, px + w, y + fold, dr::comment);
    // heading and text lines
    g.fillRect(px + 7, y + s / 4, w / 2, 4, dr::purple);
    for (int i = 0; i < 3; i++) g.fillRect(px + 7, y + s / 4 + 12 + i * 9, w - 14 - (i == 2 ? 10 : 0), 3, dr::darker);
}

void gear(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t bg) {
    const int cx = x + s / 2, cy = y + s / 2;
    const float rOuter = s * 0.44f, rBody = s * 0.33f;
    // eight teeth, each a small quad pointing outwards
    for (int i = 0; i < 8; i++) {
        const float a = i * 3.14159265f / 4, half = 0.20f;
        const int x0 = cx + (int)lroundf(cosf(a - half) * rBody), y0 = cy + (int)lroundf(sinf(a - half) * rBody);
        const int x1 = cx + (int)lroundf(cosf(a + half) * rBody), y1 = cy + (int)lroundf(sinf(a + half) * rBody);
        const int x2 = cx + (int)lroundf(cosf(a + half * 0.7f) * rOuter), y2 = cy + (int)lroundf(sinf(a + half * 0.7f) * rOuter);
        const int x3 = cx + (int)lroundf(cosf(a - half * 0.7f) * rOuter), y3 = cy + (int)lroundf(sinf(a - half * 0.7f) * rOuter);
        g.fillTriangle(x0, y0, x1, y1, x2, y2, fg);
        g.fillTriangle(x0, y0, x2, y2, x3, y3, fg);
    }
    g.fillCircle(cx, cy, (int)rBody, fg);
    g.fillCircle(cx, cy, s / 7, bg);
}

void sudoku(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t) {
    const int gap = 3, cell = (s - 2 * gap) / 3, off = (s - (3 * cell + 2 * gap)) / 2;
    static const char digits[9] = {'5', 0, '9', 0, '7', 0, '2', 0, '4'};
    g.setFont(&fonts::FreeSansBold9pt7b);
    g.setTextDatum(textdatum_t::middle_center);
    for (int i = 0; i < 9; i++) {
        const int cx = x + off + (i % 3) * (cell + gap), cy = y + off + (i / 3) * (cell + gap);
        const bool filled = digits[i] != 0;
        g.fillRoundRect(cx, cy, cell, cell, 3, filled ? fg : dr::currentLine);
        if (filled) {
            g.setTextColor(dr::darkest);
            const char str[2] = {digits[i], 0};
            g.drawString(str, cx + cell / 2, cy + cell / 2 + 1);
        }
    }
}

void crown(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t) {
    const int left = x + s / 12, right = x + s - s / 12, w = right - left;
    const int base = y + s - s / 5, bandH = s / 7, top = y + s / 5, valley = y + s / 2;
    // three points rising from the band
    g.fillTriangle(left, base, left, top + s / 10, left + w / 3, base, fg);
    g.fillTriangle(left + w / 6, base, left + w / 2, top, right - w / 6, base, fg);
    g.fillTriangle(right, base, right, top + s / 10, right - w / 3, base, fg);
    g.fillRect(left, valley + s / 8, w, base - valley - s / 8, fg);
    g.fillRoundRect(left, base, w, bandH, 2, dr::yellow);
    // jewels: tips and an ember in the middle
    g.fillCircle(left, top + s / 10, 3, dr::yellow);
    g.fillCircle(left + w / 2, top, 3, dr::yellow);
    g.fillCircle(right, top + s / 10, 3, dr::yellow);
    g.fillCircle(left + w / 2, valley + s / 6, s / 10, dr::red);
}

void letter(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t, char c) {
    g.fillRoundRect(x, y, s, s, 8, fg);
    g.setFont(&fonts::FreeSansBold18pt7b);
    g.setTextDatum(textdatum_t::middle_center);
    g.setTextColor(dr::darkest);
    const char str[2] = {c, 0};
    g.drawString(str, x + s / 2, y + s / 2 + 2);
}

}  // namespace icons
