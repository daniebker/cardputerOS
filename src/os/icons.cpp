#include "icons.h"

#include <cmath>
#include <cstring>

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

void mesh(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t) {
    // four radio nodes joined by links
    const int px[4] = {x + s / 2, x + s / 8, x + s - s / 8, x + s / 2};
    const int py[4] = {y + s / 8, y + s / 2 + s / 10, y + s / 2 + s / 10, y + s - s / 10};
    static const int links[5][2] = {{0, 1}, {0, 2}, {1, 2}, {1, 3}, {2, 3}};
    for (auto& l : links)
        for (int t = -1; t <= 1; t++) {
            g.drawLine(px[l[0]] + t, py[l[0]], px[l[1]] + t, py[l[1]], dr::comment);
            g.drawLine(px[l[0]], py[l[0]] + t, px[l[1]], py[l[1]] + t, dr::comment);
        }
    for (int i = 0; i < 4; i++) {
        g.fillCircle(px[i], py[i], s / 9, fg);
        g.fillCircle(px[i], py[i], s / 22, dr::darkest);
    }
}

void journal(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t) {
    // a bound book with a ribbon and dated lines
    const int px = x + s / 8, w = s - s / 4;
    g.fillRoundRect(px, y, w, s, 5, fg);
    g.fillRect(px, y, s / 6, s, dr::comment);            // spine
    g.fillRoundRect(px, y, s / 6, s, 3, dr::comment);
    for (int i = 0; i < 4; i++) {
        const int ly = y + s / 5 + i * (s / 6);
        g.fillRect(px + s / 6 + 5, ly, 6, 3, dr::purple);              // time
        g.fillRect(px + s / 6 + 14, ly, w - s / 6 - 20 - (i % 2) * 6, 3, dr::darker);  // text
    }
    // ribbon
    const int rx = px + w - 12;
    g.fillRect(rx, y, 6, s / 3, dr::red);
    g.fillTriangle(rx, y + s / 3, rx + 6, y + s / 3, rx + 3, y + s / 3 - 4, fg);
}

void radar(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t) {
    // a radar screen: rings, a sweep and a few blips
    const int cx = x + s / 2, cy = y + s / 2, r = s / 2 - 1;
    g.fillCircle(cx, cy, r, dr::darkest);
    g.drawCircle(cx, cy, r, fg);
    g.drawCircle(cx, cy, r * 2 / 3, dr::currentLine);
    g.drawCircle(cx, cy, r / 3, dr::currentLine);
    g.drawFastHLine(cx - r + 2, cy, 2 * r - 3, dr::currentLine);
    g.drawFastVLine(cx, cy - r + 2, 2 * r - 3, dr::currentLine);
    g.fillArc(cx, cy, 0, r - 2, 290, 340, dr::comment);  // fading trail
    g.fillArc(cx, cy, 0, r - 2, 330, 360, fg);           // the sweep
    g.fillCircle(cx + r / 2, cy - r / 4, s / 22 + 1, dr::foreground);
    g.fillCircle(cx - r / 3, cy - r / 2, s / 22 + 1, fg);
    g.fillCircle(cx - r / 2, cy + r / 3, s / 22, fg);
}

void tiles(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t) {
    // a 2x2 board of number tiles
    const int gap = 4, t = (s - 3 * gap) / 2;
    g.fillRoundRect(x, y, s, s, 7, dr::darkest);
    static const uint32_t bgs[4] = {dr::currentLine, dr::green, dr::orange, 0};
    static const char* const nums[4] = {"2", "16", "64", "2K"};
    g.setFont(&fonts::FreeSansBold9pt7b);
    g.setTextDatum(textdatum_t::middle_center);
    for (int i = 0; i < 4; i++) {
        const int tx = x + gap + (i % 2) * (t + gap), ty = y + gap + (i / 2) * (t + gap);
        g.fillRoundRect(tx, ty, t, t, 4, bgs[i] ? bgs[i] : fg);
        g.setTextColor(i == 0 ? dr::foreground : dr::darkest);
        g.drawString(nums[i], tx + t / 2, ty + t / 2 + 1);
    }
}

void folder(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t, const uint32_t* colors, int count) {
    const int top = y + s / 6, tab = s / 3;
    g.fillRoundRect(x, top, tab + 6, s / 4, 4, fg);  // tab
    g.fillRoundRect(x, top + 6, s, s - s / 6 - 6, 6, fg);
    // the apps inside: small tiles in their colours
    const int pad = 7, gap = 4, n = count < 4 ? count : 4;
    const int area = s - 2 * pad, t = (area - gap) / 2;
    const int ay = top + 6 + (s - s / 6 - 6 - (2 * t + gap)) / 2;
    for (int i = 0; i < n; i++)
        g.fillRoundRect(x + pad + (i % 2) * (t + gap), ay + (i / 2) * (t + gap), t, t, 4, colors[i]);
}

void letter(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t, char c) {
    g.fillRoundRect(x, y, s, s, 8, fg);
    g.setFont(&fonts::FreeSansBold18pt7b);
    g.setTextDatum(textdatum_t::middle_center);
    g.setTextColor(dr::darkest);
    const char str[2] = {c, 0};
    g.drawString(str, x + s / 2, y + s / 2 + 2);
}

DrawFn byName(const char* name) {
    static const struct {
        const char* name;
        DrawFn fn;
    } table[] = {
        {"terminal", terminal}, {"note", note},       {"gear", gear},   {"sudoku", sudoku}, {"crown", crown},
        {"mesh", mesh},         {"journal", journal}, {"tiles", tiles}, {"radar", radar},
    };
    for (const auto& e : table)
        if (strcmp(e.name, name) == 0) return e.fn;
    return nullptr;
}

}  // namespace icons
