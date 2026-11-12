#include "vterm.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
template <typename T>
T clampT(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

uint8_t rgbTo256(int r, int g, int b) {
    if (r == g && g == b) {
        if (r < 8) return 16;
        if (r > 248) return 231;
        return (uint8_t)(232 + (r - 8) * 24 / 247);
    }
    auto q = [](int v) { return v < 48 ? 0 : (v < 115 ? 1 : (v - 35) / 40); };
    return (uint8_t)(16 + 36 * q(r) + 6 * q(g) + q(b));
}
}  // namespace

VTerm::VTerm(int cols, int rows, int scrollbackLines) : cols_(cols), rows_(rows), sbMax_(scrollbackLines) {
    screen_.assign((size_t)cols_ * rows_, Cell());
    dirty_.assign(rows_, true);
    bot_ = rows_ - 1;
    sbCols_ = cols_;
    if (sbMax_ > 0) sb_.assign((size_t)sbMax_ * sbCols_, Cell());
}

uint32_t VTerm::paletteRgb(uint8_t i) {
    // the 16 ANSI colours: Dracula terminal palette
    static const uint32_t base[16] = {0x21222C, 0xFF5555, 0x50FA7B, 0xF1FA8C, 0xBD93F9, 0xFF79C6, 0x8BE9FD, 0xF8F8F2,
                                      0x6272A4, 0xFF6E6E, 0x69FF94, 0xFFFFA5, 0xD6ACFF, 0xFF92DF, 0xA4FFFF, 0xFFFFFF};
    if (i < 16) return base[i];
    if (i < 232) {
        static const uint8_t lv[6] = {0, 95, 135, 175, 215, 255};
        int v = i - 16;
        return ((uint32_t)lv[v / 36] << 16) | ((uint32_t)lv[(v / 6) % 6] << 8) | lv[v % 6];
    }
    uint32_t g = 8 + 10 * (i - 232);
    return (g << 16) | (g << 8) | g;
}

VTerm::Cell VTerm::blank() const {
    Cell c;
    c.ch = ' ';
    c.fg = pen_.fg;
    c.bg = pen_.bg;
    c.attr = (uint8_t)((pen_.attr & DefaultBg) | DefaultFg);
    return c;
}

const VTerm::Cell& VTerm::cell(int x, int y) const {
    static const Cell empty;
    if (x < 0 || x >= cols_ || y < 0 || y >= rows_) return empty;
    if (viewOffset_ > 0) {
        if (y < viewOffset_) {
            int idx = sbCount_ - viewOffset_ + y;  // 0 = oldest
            if (idx < 0 || x >= sbCols_) return empty;
            int slot = (sbHead_ - sbCount_ + idx + 2 * sbMax_) % sbMax_;
            return sb_[(size_t)slot * sbCols_ + x];
        }
        y -= viewOffset_;
    }
    return screen_[(size_t)y * cols_ + x];
}

void VTerm::setViewOffset(int lines) {
    lines = clampT(lines, 0, onAlt_ ? 0 : sbCount_);
    if (lines != viewOffset_) {
        viewOffset_ = lines;
        markAllDirty();
    }
}

void VTerm::clearDirty() {
    std::fill(dirty_.begin(), dirty_.end(), false);
    anyDirty_ = false;
}

void VTerm::markAllDirty() {
    std::fill(dirty_.begin(), dirty_.end(), true);
    anyDirty_ = true;
}

void VTerm::pushScrollback(const Cell* line) {
    if (sbMax_ <= 0) return;
    Cell* dst = &sb_[(size_t)sbHead_ * sbCols_];
    int n = std::min(cols_, sbCols_);
    std::copy(line, line + n, dst);
    std::fill(dst + n, dst + sbCols_, Cell());
    sbHead_ = (sbHead_ + 1) % sbMax_;
    if (sbCount_ < sbMax_) sbCount_++;
    if (viewOffset_ > 0) viewOffset_ = std::min(viewOffset_ + 1, sbCount_);
}

void VTerm::reset() {
    std::fill(screen_.begin(), screen_.end(), Cell());
    other_.clear();
    onAlt_ = false;
    cx_ = cy_ = 0;
    top_ = 0;
    bot_ = rows_ - 1;
    pen_ = Pen();
    saved_ = savedAlt_ = Saved();
    autoWrap_ = true;
    origin_ = insert_ = false;
    cursorVisible_ = true;
    appCursor_ = appKeypad_ = bracketedPaste_ = newlineMode_ = false;
    g_[0] = g_[1] = 0;
    gl_ = 0;
    state_ = State::Ground;
    utf8left_ = 0;
    viewOffset_ = 0;
    markAllDirty();
}

void VTerm::resize(int cols, int rows) {
    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;
    if (cols == cols_ && rows == rows_) return;

    // Keep the bottom of the screen (where the cursor is) when shrinking.
    int shift = std::max(0, cy_ - (rows - 1));
    auto remap = [&](std::vector<Cell>& buf, int dy) {
        if (buf.empty()) return;
        std::vector<Cell> nb((size_t)cols * rows, Cell());
        for (int y = 0; y < rows; y++) {
            int sy = y + dy;
            if (sy < 0 || sy >= rows_) continue;
            int n = std::min(cols, cols_);
            std::copy(&buf[(size_t)sy * cols_], &buf[(size_t)sy * cols_] + n, &nb[(size_t)y * cols]);
        }
        buf.swap(nb);
    };
    if (!onAlt_ && shift > 0) {
        for (int y = 0; y < shift; y++) pushScrollback(row(y));
    }
    remap(screen_, shift);
    remap(other_, 0);

    if (cols != sbCols_ && sbMax_ > 0) {
        // Re-flow is not supported: re-pack the scrollback at the new width.
        std::vector<Cell> nsb((size_t)sbMax_ * cols, Cell());
        int n = std::min(cols, sbCols_);
        for (int i = 0; i < sbCount_; i++) {
            int slot = (sbHead_ - sbCount_ + i + 2 * sbMax_) % sbMax_;
            std::copy(&sb_[(size_t)slot * sbCols_], &sb_[(size_t)slot * sbCols_] + n, &nsb[(size_t)i * cols]);
        }
        sb_.swap(nsb);
        sbHead_ = sbCount_ % sbMax_;
        sbCols_ = cols;
    }

    cols_ = cols;
    rows_ = rows;
    cy_ = clampT(cy_ - shift, 0, rows_ - 1);
    cx_ = clampT(cx_, 0, cols_ - 1);
    top_ = 0;
    bot_ = rows_ - 1;
    saved_.x = clampT(saved_.x, 0, cols_ - 1);
    saved_.y = clampT(saved_.y, 0, rows_ - 1);
    savedAlt_.x = clampT(savedAlt_.x, 0, cols_ - 1);
    savedAlt_.y = clampT(savedAlt_.y, 0, rows_ - 1);
    viewOffset_ = 0;
    dirty_.assign(rows_, true);
    anyDirty_ = true;
}

void VTerm::send(const std::string& s) {
    if (reply_) reply_(s.data(), s.size());
}

int VTerm::param(int i, int def) const {
    if (i >= nparams_ || params_[i] <= 0) return def;
    return params_[i];
}

void VTerm::moveCursor(int x, int y) {
    touch(cy_);
    cx_ = clampT(x, 0, cols_ - 1);
    cy_ = clampT(y, 0, rows_ - 1);
    touch(cy_);
}

void VTerm::scrollUp(int top, int bot, int n) {
    int h = bot - top + 1;
    if (h <= 0) return;
    n = clampT(n, 1, h);
    if (top == 0 && !onAlt_) {
        for (int i = 0; i < n; i++) pushScrollback(row(i));
    }
    if (n < h) std::copy(row(top + n), row(bot + 1), row(top));
    Cell b = blank();
    std::fill(row(bot - n + 1), row(bot + 1), b);
    for (int y = top; y <= bot; y++) touch(y);
    if (viewOffset_ > 0) markAllDirty();
}

void VTerm::scrollDown(int top, int bot, int n) {
    int h = bot - top + 1;
    if (h <= 0) return;
    n = clampT(n, 1, h);
    if (n < h) std::copy_backward(row(top), row(bot - n + 1), row(bot + 1));
    Cell b = blank();
    std::fill(row(top), row(top + n), b);
    for (int y = top; y <= bot; y++) touch(y);
}

void VTerm::lineFeed() {
    if (cy_ == bot_) {
        scrollUp(top_, bot_, 1);
    } else if (cy_ < rows_ - 1) {
        touch(cy_);
        cy_++;
        touch(cy_);
    }
}

void VTerm::reverseIndex() {
    if (cy_ == top_) {
        scrollDown(top_, bot_, 1);
    } else if (cy_ > 0) {
        touch(cy_);
        cy_--;
        touch(cy_);
    }
}

void VTerm::eraseCells(int y, int x0, int x1) {
    if (y < 0 || y >= rows_) return;
    x0 = clampT(x0, 0, cols_);
    x1 = clampT(x1, 0, cols_);
    if (x0 >= x1) return;
    Cell b = blank();
    std::fill(row(y) + x0, row(y) + x1, b);
    touch(y);
}

// Map a Unicode code point to a cell character: ASCII, a transliteration, or
// one of the procedurally drawn glyph codes. Returns 0 for "draw nothing".
uint8_t VTerm::mapCodepoint(uint32_t cp) {
    if (cp < 0x80) return (uint8_t)cp;
    if (cp < 0xA0) return 0;
    if (cp < 0x100) {
        static const char latin1[] =
            " !cLoY|S\"ca<--r-"
            "o+23'uP.,1o>????"
            "AAAAAAACEEEEIIII"
            "DNOOOOOxOUUUUYPs"
            "aaaaaaaceeeeiiii"
            "dnooooo/ouuuuypy";
        return (uint8_t)latin1[cp - 0xA0];
    }
    if (cp >= 0x0300 && cp <= 0x036F) return 0;                    // combining marks
    if ((cp >= 0x200B && cp <= 0x200F) || cp == 0xFEFF) return 0;  // zero width
    if (cp >= 0xFE00 && cp <= 0xFE0F) return 0;                    // variation selectors

    if (cp >= 0x2500 && cp <= 0x257F) {
        const uint8_t U = ArmUp, D = ArmDown, L = ArmLeft, R = ArmRight;
        uint8_t arms;
        uint32_t c = cp;
        if (c <= 0x254F) {
            if (c <= 0x250B || c >= 0x254C) {
                // dashes and solid lines: horizontal / vertical pairs
                static const uint8_t t[12] = {1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0};
                bool horiz = c >= 0x254C ? (c <= 0x254D) : t[c - 0x2500];
                arms = horiz ? (L | R) : (U | D);
            } else if (c <= 0x250F) arms = D | R;
            else if (c <= 0x2513) arms = D | L;
            else if (c <= 0x2517) arms = U | R;
            else if (c <= 0x251B) arms = U | L;
            else if (c <= 0x2523) arms = U | D | R;
            else if (c <= 0x252B) arms = U | D | L;
            else if (c <= 0x2533) arms = D | L | R;
            else if (c <= 0x253B) arms = U | L | R;
            else arms = U | D | L | R;
        } else if (c == 0x2550) arms = L | R;
        else if (c == 0x2551) arms = U | D;
        else if (c <= 0x2554) arms = D | R;
        else if (c <= 0x2557) arms = D | L;
        else if (c <= 0x255A) arms = U | R;
        else if (c <= 0x255D) arms = U | L;
        else if (c <= 0x2560) arms = U | D | R;
        else if (c <= 0x2563) arms = U | D | L;
        else if (c <= 0x2566) arms = D | L | R;
        else if (c <= 0x2569) arms = U | L | R;
        else if (c <= 0x256C) arms = U | D | L | R;
        else if (c == 0x256D) arms = D | R;
        else if (c == 0x256E) arms = D | L;
        else if (c == 0x256F) arms = U | L;
        else if (c == 0x2570) arms = U | R;
        else if (c == 0x2571) return '/';
        else if (c == 0x2572) return '\\';
        else if (c == 0x2573) return 'X';
        else if (c == 0x2574 || c == 0x2578) arms = L;
        else if (c == 0x2575 || c == 0x2579) arms = U;
        else if (c == 0x2576 || c == 0x257A) arms = R;
        else if (c == 0x2577 || c == 0x257B) arms = D;
        else arms = (c & 1) ? (U | D) : (L | R);
        return (uint8_t)(BoxBase | arms);
    }
    if (cp >= 0x2580 && cp <= 0x259F) {
        if (cp == 0x2580) return HalfUpper;
        if (cp <= 0x2587) return HalfLower;
        if (cp == 0x2588) return Block;
        if (cp <= 0x258F) return HalfLeft;
        if (cp == 0x2590) return HalfRight;
        if (cp <= 0x2593) return Shade;
        return Shade;
    }
    switch (cp) {
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-';
        case 0x2026: return '.';
        case 0x2022: case 0x00B7: case 0x2219: case 0x25CF: case 0x25CB: case 0x25E6: case 0x2027: return Bullet;
        case 0x25C6: case 0x25C7: case 0x2666: return Diamond;
        case 0x2190: case 0x25C0: case 0x2039: case 0x276E: case 0xE0B2: case 0xE0B3: return '<';
        case 0x2192: case 0x25B6: case 0x203A: case 0x276F: case 0xE0B0: case 0xE0B1: case 0x279C: return '>';
        case 0x2191: case 0x25B2: return '^';
        case 0x2193: case 0x25BC: return 'v';
        case 0x2713: case 0x2714: return 'v';
        case 0x2717: case 0x2718: case 0x00D7: return 'x';
        case 0x25A0: case 0x25AE: return Block;
        case 0x2264: return '<';
        case 0x2265: return '>';
        case 0x2260: return '#';
        case 0x20AC: return 'E';
        case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: case 0x3000: return ' ';
    }
    return '?';
}

void VTerm::putChar(uint32_t cp) {
    uint8_t ch;
    if (cp < 0x80 && g_[gl_] == 1 && cp >= 0x5F && cp <= 0x7E) {
        // DEC special graphics character set
        const uint8_t U = ArmUp, D = ArmDown, L = ArmLeft, R = ArmRight;
        switch (cp) {
            case '_': ch = ' '; break;
            case '`': ch = Diamond; break;
            case 'a': ch = Shade; break;
            case 'f': ch = 'o'; break;
            case 'g': ch = '+'; break;
            case 'j': ch = BoxBase | U | L; break;
            case 'k': ch = BoxBase | D | L; break;
            case 'l': ch = BoxBase | D | R; break;
            case 'm': ch = BoxBase | U | R; break;
            case 'n': ch = BoxBase | U | D | L | R; break;
            case 'o': case 'p': case 'q': case 'r': case 's': ch = BoxBase | L | R; break;
            case 't': ch = BoxBase | U | D | R; break;
            case 'u': ch = BoxBase | U | D | L; break;
            case 'v': ch = BoxBase | U | L | R; break;
            case 'w': ch = BoxBase | D | L | R; break;
            case 'x': ch = BoxBase | U | D; break;
            case 'y': ch = '<'; break;
            case 'z': ch = '>'; break;
            case '{': ch = '*'; break;
            case '|': ch = '#'; break;
            case '}': ch = 'L'; break;
            case '~': ch = Bullet; break;
            default: ch = (uint8_t)cp; break;
        }
    } else {
        ch = mapCodepoint(cp);
        if (ch == 0) return;
    }

    if (cx_ >= cols_) {
        if (autoWrap_) {
            cx_ = 0;
            lineFeed();
        } else {
            cx_ = cols_ - 1;
        }
    }
    Cell* r = row(cy_);
    if (insert_) std::copy_backward(r + cx_, r + cols_ - 1, r + cols_);
    Cell& c = r[cx_];
    c.ch = ch;
    c.fg = pen_.fg;
    c.bg = pen_.bg;
    c.attr = pen_.attr;
    lastCh_ = ch;
    cx_++;
    touch(cy_);
}

void VTerm::control(uint8_t c) {
    switch (c) {
        case 0x07: bell_ = true; break;
        case 0x08:
            if (cx_ >= cols_) cx_ = cols_ - 1;
            if (cx_ > 0) cx_--;
            touch(cy_);
            break;
        case 0x09: {
            int nx = ((std::min(cx_, cols_ - 1) / 8) + 1) * 8;
            cx_ = std::min(nx, cols_ - 1);
            touch(cy_);
            break;
        }
        case 0x0A: case 0x0B: case 0x0C:
            lineFeed();
            if (newlineMode_) cx_ = 0;
            break;
        case 0x0D:
            cx_ = 0;
            touch(cy_);
            break;
        case 0x0E: gl_ = 1; break;
        case 0x0F: gl_ = 0; break;
        default: break;
    }
}

void VTerm::switchScreen(bool alt, bool clear) {
    if (alt == onAlt_) {
        if (alt && clear) {
            std::fill(screen_.begin(), screen_.end(), Cell());
            markAllDirty();
        }
        return;
    }
    if (other_.size() != screen_.size()) other_.assign(screen_.size(), Cell());
    screen_.swap(other_);
    onAlt_ = alt;
    if (alt && clear) std::fill(screen_.begin(), screen_.end(), Cell());
    if (!alt) {
        // free the alternate buffer; it is recreated on demand
        std::vector<Cell>().swap(other_);
    }
    viewOffset_ = 0;
    markAllDirty();
}

void VTerm::setMode(int p, bool priv, bool on) {
    if (!priv) {
        if (p == 4) insert_ = on;
        else if (p == 20) newlineMode_ = on;
        return;
    }
    Saved& sv = onAlt_ ? savedAlt_ : saved_;
    switch (p) {
        case 1: appCursor_ = on; break;
        case 6:
            origin_ = on;
            moveCursor(0, on ? top_ : 0);
            break;
        case 7: autoWrap_ = on; break;
        case 25:
            cursorVisible_ = on;
            touch(cy_);
            break;
        case 47: case 1047: switchScreen(on, on && p == 1047); break;
        case 1048:
            if (on) {
                sv.x = cx_; sv.y = cy_; sv.pen = pen_;
            } else {
                moveCursor(sv.x, sv.y);
                pen_ = sv.pen;
            }
            break;
        case 1049:
            if (on) {
                saved_.x = cx_; saved_.y = cy_; saved_.pen = pen_;
                switchScreen(true, true);
                moveCursor(0, 0);
            } else {
                switchScreen(false, false);
                moveCursor(saved_.x, saved_.y);
                pen_ = saved_.pen;
            }
            top_ = 0;
            bot_ = rows_ - 1;
            break;
        case 2004: bracketedPaste_ = on; break;
        default: break;
    }
}

void VTerm::sgr() {
    if (nparams_ == 0) {
        pen_ = Pen();
        return;
    }
    for (int i = 0; i < nparams_; i++) {
        int p = params_[i] < 0 ? 0 : params_[i];
        if (p == 0) pen_ = Pen();
        else if (p == 1) pen_.attr |= Bold;
        else if (p == 2) pen_.attr |= Dim;
        else if (p == 4) pen_.attr |= Underline;
        else if (p == 7) pen_.attr |= Inverse;
        else if (p == 21 || p == 22) pen_.attr &= ~(Bold | Dim);
        else if (p == 24) pen_.attr &= ~Underline;
        else if (p == 27) pen_.attr &= ~Inverse;
        else if (p >= 30 && p <= 37) { pen_.fg = (uint8_t)(p - 30); pen_.attr &= ~DefaultFg; }
        else if (p == 39) { pen_.fg = 7; pen_.attr |= DefaultFg; }
        else if (p >= 40 && p <= 47) { pen_.bg = (uint8_t)(p - 40); pen_.attr &= ~DefaultBg; }
        else if (p == 49) { pen_.bg = 0; pen_.attr |= DefaultBg; }
        else if (p >= 90 && p <= 97) { pen_.fg = (uint8_t)(p - 90 + 8); pen_.attr &= ~DefaultFg; }
        else if (p >= 100 && p <= 107) { pen_.bg = (uint8_t)(p - 100 + 8); pen_.attr &= ~DefaultBg; }
        else if (p == 38 || p == 48) {
            int col = -1;
            int mode = (i + 1 < nparams_) ? params_[i + 1] : -1;
            if (mode == 5 && i + 2 < nparams_) {
                col = clampT(params_[i + 2], 0, 255);
                i += 2;
            } else if (mode == 2 && i + 4 < nparams_) {
                // 38;2;r;g;b  (also tolerates the 38:2::r:g:b form with an empty colourspace)
                int k = i + 2;
                if (i + 5 < nparams_ && params_[k] < 0) k++;
                col = rgbTo256(clampT(params_[k], 0, 255), clampT(params_[k + 1], 0, 255),
                               clampT(params_[k + 2], 0, 255));
                i = k + 2;
            } else {
                break;  // malformed
            }
            if (p == 38) { pen_.fg = (uint8_t)col; pen_.attr &= ~DefaultFg; }
            else { pen_.bg = (uint8_t)col; pen_.attr &= ~DefaultBg; }
        }
    }
}

void VTerm::escDispatch(uint8_t c) {
    if (inter_ == '(' || inter_ == ')') {
        g_[inter_ == '(' ? 0 : 1] = (c == '0') ? 1 : 0;
        return;
    }
    if (inter_) return;  // ESC # 8, ESC % G, ESC SP F ... ignored
    Saved& sv = onAlt_ ? savedAlt_ : saved_;
    switch (c) {
        case '7':
            sv.x = cursorX(); sv.y = cy_; sv.pen = pen_;
            sv.g[0] = g_[0]; sv.g[1] = g_[1]; sv.gl = gl_; sv.origin = origin_;
            break;
        case '8':
            moveCursor(sv.x, sv.y);
            pen_ = sv.pen;
            g_[0] = sv.g[0]; g_[1] = sv.g[1]; gl_ = sv.gl; origin_ = sv.origin;
            break;
        case 'D': lineFeed(); break;
        case 'E': lineFeed(); cx_ = 0; break;
        case 'M': reverseIndex(); break;
        case 'c': reset(); break;
        case '=': appKeypad_ = true; break;
        case '>': appKeypad_ = false; break;
        case 'Z': send("\x1b[?1;2c"); break;
        default: break;
    }
}

void VTerm::csiDispatch(uint8_t c) {
    const int n = param(0, 1);
    const int x = cursorX();
    if (priv_ && c != 'h' && c != 'l' && c != 'c' && c != 'n') return;  // unknown private sequences
    if (inter_) {
        if (inter_ == '!' && c == 'p') {  // DECSTR soft reset
            pen_ = Pen();
            insert_ = origin_ = false;
            autoWrap_ = true;
            cursorVisible_ = true;
            appCursor_ = false;
            top_ = 0;
            bot_ = rows_ - 1;
            g_[0] = g_[1] = 0;
            gl_ = 0;
        }
        return;  // DECSCUSR etc. ignored
    }
    switch (c) {
        case '@': {
            int k = std::min(n, cols_ - x);
            Cell* r = row(cy_);
            std::copy_backward(r + x, r + cols_ - k, r + cols_);
            std::fill(r + x, r + x + k, blank());
            touch(cy_);
            break;
        }
        case 'A': moveCursor(x, std::max(cy_ - n, cy_ >= top_ ? top_ : 0)); break;
        case 'B': case 'e': moveCursor(x, std::min(cy_ + n, cy_ <= bot_ ? bot_ : rows_ - 1)); break;
        case 'C': case 'a': moveCursor(x + n, cy_); break;
        case 'D': moveCursor(x - n, cy_); break;
        case 'E': moveCursor(0, std::min(cy_ + n, cy_ <= bot_ ? bot_ : rows_ - 1)); break;
        case 'F': moveCursor(0, std::max(cy_ - n, cy_ >= top_ ? top_ : 0)); break;
        case 'G': case '`': moveCursor(n - 1, cy_); break;
        case 'H': case 'f': {
            int r = param(0, 1) - 1, col = param(1, 1) - 1;
            if (origin_) r = std::min(r + top_, bot_);
            moveCursor(col, r);
            break;
        }
        case 'd': {
            int r = n - 1;
            if (origin_) r = std::min(r + top_, bot_);
            moveCursor(x, r);
            break;
        }
        case 'J': {
            int p = param(0, 0);
            if (p == 0) {
                eraseCells(cy_, x, cols_);
                for (int y = cy_ + 1; y < rows_; y++) eraseCells(y, 0, cols_);
            } else if (p == 1) {
                for (int y = 0; y < cy_; y++) eraseCells(y, 0, cols_);
                eraseCells(cy_, 0, x + 1);
            } else if (p == 2) {
                for (int y = 0; y < rows_; y++) eraseCells(y, 0, cols_);
            } else if (p == 3) {
                sbCount_ = 0;
                sbHead_ = 0;
                setViewOffset(0);
            }
            break;
        }
        case 'K': {
            int p = param(0, 0);
            if (p == 0) eraseCells(cy_, x, cols_);
            else if (p == 1) eraseCells(cy_, 0, x + 1);
            else if (p == 2) eraseCells(cy_, 0, cols_);
            break;
        }
        case 'L':
            if (cy_ >= top_ && cy_ <= bot_) {
                scrollDown(cy_, bot_, n);
                cx_ = x;  // only cancels a pending wrap
            }
            break;
        case 'M':
            if (cy_ >= top_ && cy_ <= bot_) {
                // deleting lines must not feed the scrollback
                bool alt = onAlt_;
                onAlt_ = true;
                scrollUp(cy_, bot_, n);
                onAlt_ = alt;
                cx_ = x;
            }
            break;
        case 'P': {
            int k = std::min(n, cols_ - x);
            Cell* r = row(cy_);
            std::copy(r + x + k, r + cols_, r + x);
            std::fill(r + cols_ - k, r + cols_, blank());
            touch(cy_);
            break;
        }
        case 'X': eraseCells(cy_, x, x + n); break;
        case 'S': scrollUp(top_, bot_, n); break;
        case 'T': scrollDown(top_, bot_, n); break;
        case 'b': {
            int k = std::min(n, cols_ * rows_);
            uint8_t ch = lastCh_;
            for (int i = 0; i < k; i++) {
                // lastCh_ is already a mapped cell character; bypass re-mapping
                if (cx_ >= cols_) {
                    if (!autoWrap_) break;
                    cx_ = 0;
                    lineFeed();
                }
                Cell& cl = row(cy_)[cx_];
                cl.ch = ch; cl.fg = pen_.fg; cl.bg = pen_.bg; cl.attr = pen_.attr;
                cx_++;
                touch(cy_);
            }
            break;
        }
        case 'c':
            if (priv_ == '>') send("\x1b[>0;10;1c");
            else if (!priv_ && param(0, 0) == 0) send("\x1b[?1;2c");
            break;
        case 'h': case 'l':
            for (int i = 0; i < nparams_; i++) setMode(params_[i], priv_ == '?', c == 'h');
            break;
        case 'm': sgr(); break;
        case 'n':
            if (priv_) break;
            if (param(0, 0) == 5) send("\x1b[0n");
            else if (param(0, 0) == 6) {
                char buf[40];
                snprintf(buf, sizeof buf, "\x1b[%d;%dR", (origin_ ? cy_ - top_ : cy_) + 1, x + 1);
                send(buf);
            }
            break;
        case 'r': {
            int t = param(0, 1) - 1, b = param(1, rows_) - 1;
            t = clampT(t, 0, rows_ - 1);
            b = clampT(b, 0, rows_ - 1);
            if (t < b) {
                top_ = t;
                bot_ = b;
            } else {
                top_ = 0;
                bot_ = rows_ - 1;
            }
            moveCursor(0, origin_ ? top_ : 0);
            break;
        }
        case 's': {
            Saved& sv = onAlt_ ? savedAlt_ : saved_;
            sv.x = x; sv.y = cy_; sv.pen = pen_;
            break;
        }
        case 'u': {
            Saved& sv = onAlt_ ? savedAlt_ : saved_;
            moveCursor(sv.x, sv.y);
            pen_ = sv.pen;
            break;
        }
        case 't':
            if (param(0, 0) == 18) {
                char buf[40];
                snprintf(buf, sizeof buf, "\x1b[8;%d;%dt", rows_, cols_);
                send(buf);
            }
            break;
        default: break;
    }
}

void VTerm::oscDispatch() {
    // OSC 0 / 2 ; title
    size_t semi = osc_.find(';');
    if (semi == std::string::npos) return;
    int code = atoi(osc_.c_str());
    if (code == 0 || code == 2) title_ = osc_.substr(semi + 1);
}

void VTerm::write(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)data[i];

        // String states swallow everything until their terminator.
        if (state_ == State::Osc || state_ == State::Str) {
            if (c == 0x07) {
                if (state_ == State::Osc) oscDispatch();
                state_ = State::Ground;
            } else if (c == 0x1B) {
                state_ = (state_ == State::Osc) ? State::OscEsc : State::StrEsc;
            } else if (c == 0x18 || c == 0x1A) {
                state_ = State::Ground;
            } else if (state_ == State::Osc && osc_.size() < 256) {
                osc_.push_back((char)c);
            }
            continue;
        }
        if (state_ == State::OscEsc || state_ == State::StrEsc) {
            if (state_ == State::OscEsc) oscDispatch();
            state_ = State::Ground;
            if (c == '\\') continue;
            // not a string terminator: treat the ESC as the start of a new sequence
            state_ = State::Esc;
            inter_ = 0;
        }

        if (c == 0x1B) {
            state_ = State::Esc;
            inter_ = 0;
            utf8left_ = 0;
            continue;
        }
        if (c == 0x18 || c == 0x1A) {
            state_ = State::Ground;
            continue;
        }
        if (c < 0x20) {
            utf8left_ = 0;
            control(c);
            continue;
        }
        if (c == 0x7F) continue;

        switch (state_) {
            case State::Ground:
                if (c < 0x80) {
                    utf8left_ = 0;
                    putChar(c);
                } else if (utf8left_ > 0 && (c & 0xC0) == 0x80) {
                    utf8cp_ = (utf8cp_ << 6) | (c & 0x3F);
                    if (--utf8left_ == 0) putChar(utf8cp_);
                } else if ((c & 0xE0) == 0xC0) {
                    utf8cp_ = c & 0x1F;
                    utf8left_ = 1;
                } else if ((c & 0xF0) == 0xE0) {
                    utf8cp_ = c & 0x0F;
                    utf8left_ = 2;
                } else if ((c & 0xF8) == 0xF0) {
                    utf8cp_ = c & 0x07;
                    utf8left_ = 3;
                } else {
                    utf8left_ = 0;
                    putChar('?');
                }
                break;

            case State::Esc:
                if (c >= 0x20 && c <= 0x2F) {
                    inter_ = (char)c;
                    state_ = State::EscInter;
                } else if (c == '[') {
                    state_ = State::Csi;
                    nparams_ = 0;
                    paramSeen_ = false;
                    priv_ = 0;
                    inter_ = 0;
                    for (int k = 0; k < kMaxParams; k++) params_[k] = -1;
                } else if (c == ']') {
                    state_ = State::Osc;
                    osc_.clear();
                } else if (c == 'P' || c == 'X' || c == '^' || c == '_') {
                    state_ = State::Str;
                } else {
                    state_ = State::Ground;
                    escDispatch(c);
                }
                break;

            case State::EscInter:
                if (c >= 0x20 && c <= 0x2F) {
                    // keep the first intermediate
                } else {
                    state_ = State::Ground;
                    escDispatch(c);
                }
                break;

            case State::Csi:
                if (c >= '0' && c <= '9') {
                    if (nparams_ == 0) nparams_ = 1;
                    int& p = params_[nparams_ - 1];
                    if (p < 0) p = 0;
                    if (p < 100000) p = p * 10 + (c - '0');
                    paramSeen_ = true;
                } else if (c == ';' || c == ':') {
                    if (nparams_ == 0) nparams_ = 1;
                    if (nparams_ < kMaxParams) nparams_++;
                } else if (c >= 0x3C && c <= 0x3F) {
                    priv_ = (char)c;
                } else if (c >= 0x20 && c <= 0x2F) {
                    inter_ = (char)c;
                } else if (c >= 0x40 && c <= 0x7E) {
                    state_ = State::Ground;
                    csiDispatch(c);
                } else {
                    state_ = State::Ground;
                }
                break;

            default:
                state_ = State::Ground;
                break;
        }
    }
}

std::string VTerm::encodeKey(const key::Event& ev) const {
    using namespace key;
    std::string out;
    if (ev.mods & Opt) return out;
    const bool alt = ev.mods & Alt, ctrl = ev.mods & Ctrl, shift = ev.mods & Shift;
    uint16_t c = ev.code;

    if (c < 0x80) {
        char b = (char)c;
        if (c == Enter) b = '\r';
        else if (c == Backspace) b = ctrl ? '\x08' : '\x7f';
        else if (c == Tab) {
            if (shift) return "\x1b[Z";
        } else if (c == Esc) {
        } else if (c >= 0x20 && ctrl) {
            if (c >= '@' && c <= '_') b = (char)(c & 0x1F);
            else if (c >= 'a' && c <= 'z') b = (char)(c & 0x1F);
            else if (c == ' ' || c == '2') b = 0;
            else if (c >= '3' && c <= '7') b = (char)(0x1B + (c - '3'));
            else if (c == '8' || c == '?') b = 0x7F;
            else if (c == '/') b = 0x1F;
        } else if (c < 0x20) {
            return out;
        }
        if (alt) out.push_back('\x1b');
        out.push_back(b);
        return out;
    }

    int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    char buf[16];
    auto letter = [&](char l) {
        if (mod > 1) snprintf(buf, sizeof buf, "\x1b[1;%d%c", mod, l);
        else snprintf(buf, sizeof buf, "\x1b%c%c", appCursor_ ? 'O' : '[', l);
        return std::string(buf);
    };
    auto tilde = [&](int n) {
        if (mod > 1) snprintf(buf, sizeof buf, "\x1b[%d;%d~", n, mod);
        else snprintf(buf, sizeof buf, "\x1b[%d~", n);
        return std::string(buf);
    };
    switch (c) {
        case Up: return letter('A');
        case Down: return letter('B');
        case Right: return letter('C');
        case Left: return letter('D');
        case Home: return letter('H');
        case End: return letter('F');
        case PageUp: return tilde(5);
        case PageDown: return tilde(6);
        case Delete: return tilde(3);
        case F1: case F2: case F3: case F4:
            if (mod > 1) snprintf(buf, sizeof buf, "\x1b[1;%d%c", mod, 'P' + (c - F1));
            else snprintf(buf, sizeof buf, "\x1bO%c", 'P' + (c - F1));
            return buf;
        case F5: return tilde(15);
        case F6: return tilde(17);
        case F7: return tilde(18);
        case F8: return tilde(19);
        case F9: return tilde(20);
        case F10: return tilde(21);
        default: return out;
    }
}
