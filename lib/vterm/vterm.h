// VTerm — a small, portable xterm-ish terminal emulator (no platform dependencies).
// Feed it bytes from the host with write(); read the cell grid to render it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "key.h"

class VTerm {
public:
    enum Attr : uint8_t {
        Bold = 1,
        Underline = 2,
        Inverse = 4,
        DefaultFg = 8,
        DefaultBg = 16,
        Dim = 32,
    };

    struct Cell {
        uint8_t ch = ' ';
        uint8_t fg = 7;  // xterm 256-colour palette index
        uint8_t bg = 0;
        uint8_t attr = DefaultFg | DefaultBg;
        bool operator==(const Cell& o) const { return ch == o.ch && fg == o.fg && bg == o.bg && attr == o.attr; }
    };

    // Cell::ch values >= 0x80 are procedurally drawn glyphs, not font characters.
    // 0x80..0x8F: box-drawing lines, low nibble = arms present.
    enum Glyph : uint8_t {
        BoxBase = 0x80,
        ArmUp = 1,
        ArmDown = 2,
        ArmLeft = 4,
        ArmRight = 8,
        Block = 0x90,
        Shade = 0x91,
        Bullet = 0x92,
        Diamond = 0x93,
        HalfLower = 0x94,
        HalfUpper = 0x95,
        HalfLeft = 0x96,
        HalfRight = 0x97,
    };

    using ReplyFn = std::function<void(const char* data, size_t len)>;

    VTerm(int cols, int rows, int scrollbackLines = 0);

    void write(const char* data, size_t len);
    void write(const std::string& s) { write(s.data(), s.size()); }
    void resize(int cols, int rows);
    void reset();

    // Bytes the terminal needs to send back to the host (cursor reports etc).
    void onReply(ReplyFn fn) { reply_ = std::move(fn); }

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    int cursorX() const { return cx_ >= cols_ ? cols_ - 1 : cx_; }
    int cursorY() const { return cy_; }
    bool cursorVisible() const { return cursorVisible_ && viewOffset_ == 0; }
    bool appCursorKeys() const { return appCursor_; }
    bool bracketedPaste() const { return bracketedPaste_; }
    bool altScreen() const { return onAlt_; }
    const std::string& title() const { return title_; }

    // Cell as currently displayed (takes the scrollback view offset into account).
    const Cell& cell(int x, int y) const;

    // Scrollback view: 0 = live screen, n = scrolled n lines back.
    int scrollbackSize() const { return sbCount_; }
    int viewOffset() const { return viewOffset_; }
    void setViewOffset(int lines);
    void scrollView(int delta) { setViewOffset(viewOffset_ + delta); }

    // Dirty tracking for the renderer.
    bool anyDirty() const { return anyDirty_; }
    bool rowDirty(int y) const { return dirty_[y]; }
    void clearDirty();
    void markAllDirty();

    // Bell rang since last call?
    bool takeBell() {
        bool b = bell_;
        bell_ = false;
        return b;
    }

    // Translate a key event into the byte sequence a terminal would send.
    std::string encodeKey(const key::Event& ev) const;

    // Maps a Unicode code point to a cell character: ASCII, an ASCII look-alike,
    // one of the Glyph codes, or 0 for "invisible".
    static uint8_t glyphFor(uint32_t codepoint) { return mapCodepoint(codepoint); }

    // xterm 256-colour palette entry as 0xRRGGBB.
    static uint32_t paletteRgb(uint8_t index);

private:
    enum class State : uint8_t { Ground, Esc, EscInter, Csi, Osc, OscEsc, Str, StrEsc };

    struct Pen {
        uint8_t fg = 7, bg = 0, attr = DefaultFg | DefaultBg;
    };
    struct Saved {
        int x = 0, y = 0;
        Pen pen;
        uint8_t g[2] = {0, 0};
        uint8_t gl = 0;
        bool origin = false;
    };

    Cell* row(int y) { return &screen_[(size_t)y * cols_]; }
    const Cell* row(int y) const { return &screen_[(size_t)y * cols_]; }
    Cell blank() const;
    void touch(int y) {
        if (y >= 0 && y < rows_) {
            dirty_[y] = true;
            anyDirty_ = true;
        }
    }
    void moveCursor(int x, int y);
    void putChar(uint32_t cp);
    void control(uint8_t c);
    void lineFeed();
    void reverseIndex();
    void scrollUp(int top, int bot, int n);
    void scrollDown(int top, int bot, int n);
    void eraseCells(int y, int x0, int x1);
    void escDispatch(uint8_t c);
    void csiDispatch(uint8_t c);
    void oscDispatch();
    void setMode(int p, bool priv, bool on);
    void sgr();
    void switchScreen(bool alt, bool clear);
    void pushScrollback(const Cell* line);
    int param(int i, int def) const;
    void send(const std::string& s);
    static uint8_t mapCodepoint(uint32_t cp);

    int cols_, rows_;
    std::vector<Cell> screen_, other_;  // other_ holds the inactive (main/alt) screen
    std::vector<bool> dirty_;
    bool anyDirty_ = true;

    int cx_ = 0, cy_ = 0;
    int top_ = 0, bot_ = 0;  // scroll region, inclusive
    Pen pen_;
    Saved saved_, savedAlt_;
    bool onAlt_ = false;
    bool autoWrap_ = true, origin_ = false, insert_ = false;
    bool cursorVisible_ = true, appCursor_ = false, appKeypad_ = false, bracketedPaste_ = false;
    bool newlineMode_ = false;
    uint8_t g_[2] = {0, 0};  // 0 = ASCII, 1 = DEC special graphics
    uint8_t gl_ = 0;
    uint8_t lastCh_ = ' ';
    bool bell_ = false;

    // parser
    State state_ = State::Ground;
    static const int kMaxParams = 16;
    int params_[kMaxParams];
    int nparams_ = 0;
    bool paramSeen_ = false;
    char priv_ = 0;    // '?', '>' , '=' or 0
    char inter_ = 0;   // intermediate byte
    std::string osc_;
    uint32_t utf8cp_ = 0;
    int utf8left_ = 0;

    // scrollback ring (main screen only)
    int sbMax_ = 0, sbCount_ = 0, sbHead_ = 0, sbCols_ = 0;
    std::vector<Cell> sb_;
    int viewOffset_ = 0;

    std::string title_;
    ReplyFn reply_;
};
