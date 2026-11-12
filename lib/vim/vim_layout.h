// Screen layout for the Vim engine with word-wrapped lines: which part of which
// line each screen row shows, keeping the cursor visible. Portable, no drawing.
#pragma once
#include <vector>

#include "vim.h"

struct VimRow {
    int line = -1;   // -1: past the end of the text (drawn as "~")
    int start = 0;   // first column of the line shown in this row
    int len = 0;     // number of characters shown
    int cursor = -1; // cursor column within the row, or -1
};

class VimLayout {
public:
    // Recomputes the rows for a screen of cols x rows cells, scrolling as needed.
    const std::vector<VimRow>& update(const Vim& vim, int cols, int rows);
    const std::vector<VimRow>& rows() const { return rows_; }
    void reset() { top_ = topSub_ = 0; }

private:
    int rowsOf(const Vim& vim, int line, int cols) const;
    void segments(const Vim& vim, int line, int cols, std::vector<int>& starts) const;
    int top_ = 0, topSub_ = 0;
    std::vector<VimRow> rows_;
};
