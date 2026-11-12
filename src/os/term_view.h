// Renders a VTerm cell grid onto the display, one text row at a time.
#pragma once
#include <M5GFX.h>

#include "app.h"
#include "vterm.h"

namespace os {

class TermView {
public:
    // Cell sizes: Small 4x6 (80x37 on the 320x240 screen), Normal 6x8 (53x28),
    // Medium 8x10 (40x22), Large 8x16 (40x14).
    enum Font { Small = 0, Normal = 1, Medium = 2, Large = 3 };
    static constexpr int kFontCount = 4;
    static const char* fontName(Font f);

    // The saved terminal font setting (Medium if never set).
    static Font savedFont();
    static void saveFont(Font f);

    void setFont(Font f);
    Font font() const { return font_; }
    int cellW() const { return cw_; }
    int cellH() const { return ch_; }
    int colsFor(const Rect& r) const { return r.w / cw_; }
    int rowsFor(const Rect& r) const { return r.h / ch_; }

    // Paints rows that changed (or everything if `full`), then clears the dirty flags.
    void render(Gfx& g, VTerm& term, const Rect& r, bool full);

private:
    void drawRow(VTerm& term, int y);
    Font font_ = Normal;
    int cw_ = 6, ch_ = 8, glyphDy_ = 0;
    const lgfx::IFont* glyphs_ = &fonts::Font0;
    M5Canvas row_;
    int rowW_ = 0, rowH_ = 0;
};

}  // namespace os
