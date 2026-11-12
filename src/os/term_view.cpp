#include "term_view.h"

#include "storage.h"
#include "theme.h"

namespace os {

namespace {
constexpr uint32_t kDefaultFg = theme::termFg;
constexpr uint32_t kDefaultBg = theme::termBg;
constexpr uint32_t kCursor = theme::termCursor;
}  // namespace

const char* TermView::fontName(Font f) {
    switch (f) {
        case Small: return "Small 4x6 (80 columns)";
        case Normal: return "Normal 6x8 (53 columns)";
        case Medium: return "Medium 8x10 (40 columns)";
        default: return "Large 8x16 (40 columns, 14 rows)";
    }
}

TermView::Font TermView::savedFont() {
    int v = storage::getInt("term_size", Medium);
    return (v >= 0 && v < kFontCount) ? (Font)v : Medium;
}

void TermView::saveFont(Font f) { storage::putInt("term_size", (int)f); }

void TermView::setFont(Font f) {
    font_ = f;
    switch (f) {
        // TomThumb is a GFX font: it is drawn from its baseline, not its top edge
        case Small: cw_ = 4; ch_ = 6; glyphDy_ = 5; glyphs_ = &fonts::TomThumb; break;
        case Normal: cw_ = 6; ch_ = 8; glyphDy_ = 0; glyphs_ = &fonts::Font0; break;
        case Medium: cw_ = 8; ch_ = 10; glyphDy_ = 1; glyphs_ = &fonts::Font8x8C64; break;
        default: cw_ = 8; ch_ = 16; glyphDy_ = 0; glyphs_ = &fonts::AsciiFont8x16; break;
    }
}

void TermView::drawRow(VTerm& term, int y) {
    const int cols = term.cols();
    const bool cursorRow = term.cursorVisible() && term.cursorY() == y;
    row_.fillSprite(kDefaultBg);
    for (int x = 0; x < cols; x++) {
        const VTerm::Cell& c = term.cell(x, y);
        uint8_t fgi = c.fg;
        if ((c.attr & VTerm::Bold) && !(c.attr & VTerm::DefaultFg) && fgi < 8) fgi += 8;
        uint32_t fg = (c.attr & VTerm::DefaultFg) ? ((c.attr & VTerm::Bold) ? 0xFFFFFF : kDefaultFg)
                                                  : VTerm::paletteRgb(fgi);
        uint32_t bg = (c.attr & VTerm::DefaultBg) ? kDefaultBg : VTerm::paletteRgb(c.bg);
        if ((c.attr & VTerm::Dim) && (c.attr & VTerm::DefaultFg)) fg = theme::dracula::comment;
        if (c.attr & VTerm::Inverse) std::swap(fg, bg);
        if (cursorRow && x == term.cursorX()) {
            bg = kCursor;
            fg = kDefaultBg;
        }
        const int px = x * cw_;
        if (bg != kDefaultBg) row_.fillRect(px, 0, cw_, ch_, bg);
        const uint8_t ch = c.ch;
        if (ch > ' ' && ch < 0x7F) {
            row_.setTextColor(fg);
            row_.drawChar(ch, px, glyphDy_);
        } else if (ch >= VTerm::BoxBase && ch <= (VTerm::BoxBase | 0x0F)) {
            const int mx = px + cw_ / 2, my = ch_ / 2;
            if (ch & VTerm::ArmUp) row_.drawFastVLine(mx, 0, my + 1, fg);
            if (ch & VTerm::ArmDown) row_.drawFastVLine(mx, my, ch_ - my, fg);
            if (ch & VTerm::ArmLeft) row_.drawFastHLine(px, my, cw_ / 2 + 1, fg);
            if (ch & VTerm::ArmRight) row_.drawFastHLine(mx, my, cw_ - cw_ / 2, fg);
        } else {
            switch (ch) {
                case VTerm::Block: row_.fillRect(px, 0, cw_, ch_, fg); break;
                case VTerm::Shade:
                    for (int yy = 0; yy < ch_; yy++)
                        for (int xx = (yy & 1); xx < cw_; xx += 2) row_.drawPixel(px + xx, yy, fg);
                    break;
                case VTerm::Bullet: row_.fillRect(px + cw_ / 2 - 1, ch_ / 2 - 1, 2, 2, fg); break;
                case VTerm::Diamond:
                    row_.drawFastHLine(px + cw_ / 2 - 1, ch_ / 2, 3, fg);
                    row_.drawFastVLine(px + cw_ / 2, ch_ / 2 - 1, 3, fg);
                    break;
                case VTerm::HalfLower: row_.fillRect(px, ch_ / 2, cw_, ch_ - ch_ / 2, fg); break;
                case VTerm::HalfUpper: row_.fillRect(px, 0, cw_, ch_ / 2, fg); break;
                case VTerm::HalfLeft: row_.fillRect(px, 0, cw_ / 2, ch_, fg); break;
                case VTerm::HalfRight: row_.fillRect(px + cw_ / 2, 0, cw_ - cw_ / 2, ch_, fg); break;
                default: break;
            }
        }
        if (c.attr & VTerm::Underline) row_.drawFastHLine(px, ch_ - 1, cw_, fg);
    }
}

void TermView::render(Gfx& g, VTerm& term, const Rect& r, bool full) {
    if (!full && !term.anyDirty()) return;
    if (rowW_ != r.w || rowH_ != ch_ || !row_.getBuffer()) {
        row_.deleteSprite();
        row_.setColorDepth(16);
        row_.createSprite(r.w, ch_);
        rowW_ = r.w;
        rowH_ = ch_;
    }
    row_.setFont(glyphs_);
    row_.setTextDatum(textdatum_t::top_left);
    row_.setTextSize(1);

    const int rows = term.rows();
    g.startWrite();
    for (int y = 0; y < rows; y++) {
        if (!full && !term.rowDirty(y)) continue;
        // pushSprite sends the row in the background (DMA): wait until the previous
        // row has been sent before drawing into the same buffer again
        g.waitDMA();
        drawRow(term, y);
        row_.pushSprite(&g, r.x, r.y + y * ch_);
    }
    g.waitDMA();
    if (full) {
        // clear the strip below the last text row
        const int used = rows * ch_;
        if (used < r.h) g.fillRect(r.x, r.y + used, r.w, r.h - used, kDefaultBg);
    }
    g.endWrite();
    term.clearDirty();
}

}  // namespace os
