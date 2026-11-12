// Vim — a small modal text editor engine with vi key bindings (portable, no
// platform dependencies). It owns the text and the cursor; drawing is up to the caller.
//
// Supported: counts, h j k l w b e W B E 0 ^ $ gg G { } f F t T ; ,  / ? n N *
// operators d c y > < with motions and text objects (iw aw i" a" i' a'),
// dd cc yy D C Y x X s S r J ~ p P u ctrl-r .  i a I A o O, visual v / V,
// ctrl-d ctrl-u ctrl-f ctrl-b, :w :q :q! :wq :x :<line>, ZZ ZQ.
// ctrl-[ and ctrl-c work as Esc.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "key.h"

class Vim {
public:
    enum class Mode { Normal, Insert, Visual, VisualLine, Command, Search };
    enum class Request { None, Write, Quit, WriteQuit, ForceQuit };
    using Line = std::u16string;
    struct Pos {
        int r = 0, c = 0;
    };

    Vim();

    void setText(const std::string& utf8);
    std::string text() const;

    Request onKey(const key::Event& ev);

    const std::vector<Line>& lines() const { return lines_; }
    int row() const { return cur_.r; }
    int col() const { return cur_.c; }
    Mode mode() const { return mode_; }
    bool modified() const { return modified_; }
    void setModified(bool m) { modified_ = m; }
    // Number of edits since the text was loaded (changes on undo too).
    uint32_t revision() const { return revision_; }

    // The ":" or "/" line being typed, or empty.
    std::string prompt() const;
    const std::string& message() const { return message_; }
    void setMessage(const std::string& m) { message_ = m; }

    // Visual selection, normalised (a <= b, both inclusive). False if none.
    bool selection(Pos& a, Pos& b, bool& linewise) const;

    // Rows on screen, used by ctrl-d / ctrl-u / ctrl-f / ctrl-b.
    void setPageRows(int rows) { pageRows_ = rows > 2 ? rows : 2; }

    static std::string toUtf8(const Line& l);
    static Line fromUtf8(const std::string& s);

private:
    struct Motion {
        Pos to;
        bool ok = false, linewise = false, inclusive = false;
    };
    struct UndoEntry {
        int row = 0;
        std::vector<Line> oldLines, newLines;
        Pos before, after;
    };

    Request handle(const key::Event& ev);
    Request normalKey(const key::Event& ev);
    void insertKey(const key::Event& ev);
    Request promptKey(const key::Event& ev);
    Request runCommand(const std::string& cmd);

    bool motion(uint16_t k, int count, bool hasCount, bool forOp, Motion& m);
    bool textObject(uint16_t kind, bool around, Pos& a, Pos& b);
    void applyOperator(char op, Pos from, const Motion& m);
    void applyLinewise(char op, int r0, int r1);
    void applyCharwise(char op, Pos a, Pos b);  // b exclusive
    void visualOperator(char op);
    void put(bool after, int count);
    void joinLines(int count);
    void shiftLines(int r0, int r1, bool right);
    void enterInsert();
    void leaveInsert();
    void search(bool forward, bool fromCursor);
    bool findChar(uint16_t kind, uint16_t ch, int count, Motion& m);

    // primitives
    int len(int r) const { return (int)lines_[r].size(); }
    int lastRow() const { return (int)lines_.size() - 1; }
    int firstNonBlank(int r) const;
    void clamp();
    void touch();  // call before every mutation
    Line cut(Pos a, Pos b, bool remove);  // text in [a, b)
    void insertAt(Pos p, const Line& text, Pos* end);
    Pos wordFwd(Pos p, bool big) const;
    Pos wordBack(Pos p, bool big) const;
    Pos wordEnd(Pos p, bool big) const;
    void commitUndo();
    void undo();
    void redo();
    void resetPending();
    bool idle() const;

    std::vector<Line> lines_;
    Pos cur_;
    int wantCol_ = 0;
    Mode mode_ = Mode::Normal;
    bool modified_ = false;
    uint32_t revision_ = 0;
    int pageRows_ = 20;

    // pending normal-mode command
    int count_ = 0, opCount_ = 0;
    bool hasCount_ = false;
    char op_ = 0;
    uint16_t prefix_ = 0;  // g, f, t, F, T, r, Z, i, a (text object)

    Pos anchor_;  // visual mode
    std::string cmd_;  // ":" / "/" input
    bool searchForward_ = true;
    Line pattern_;
    bool patternForward_ = true;
    uint16_t lastFindKind_ = 0, lastFindChar_ = 0;

    Line reg_;
    bool regLinewise_ = false;

    // undo
    bool snap_ = false;
    std::vector<Line> snapLines_;
    Pos snapCur_;
    std::vector<UndoEntry> undo_, redo_;

    // dot repeat
    std::vector<key::Event> rec_, lastChange_;
    uint32_t recTick_ = 0, tick_ = 0;
    bool replaying_ = false;

    std::string message_;
};
