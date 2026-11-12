#include "vim_layout.h"

#include <algorithm>

// Splits a line into screen rows, breaking after the last space that fits
// (word wrap) and only inside a word when the word is longer than a row.
// `cursorAtEnd`: the cursor sits just past the last character (insert mode)
// and needs a cell of its own.
static void wrapLine(const Vim::Line& l, int cols, bool cursorAtEnd, std::vector<int>& starts) {
    starts.clear();
    starts.push_back(0);
    const int n = (int)l.size();
    int start = 0;
    while (n - start > cols) {
        int brk = -1;
        for (int k = start + cols; k > start; k--) {
            if (l[k - 1] == ' ') {
                brk = k;  // the row ends with (and includes) this space
                break;
            }
        }
        if (brk < 0) brk = start + cols;  // one word longer than a row: hard break
        start = brk;
        starts.push_back(start);
    }
    if (cursorAtEnd && n - start == cols) starts.push_back(n);
}

void VimLayout::segments(const Vim& vim, int line, int cols, std::vector<int>& starts) const {
    const bool cursorHere = line == vim.row() && vim.mode() == Vim::Mode::Insert &&
                            vim.col() >= (int)vim.lines()[line].size();
    wrapLine(vim.lines()[line], cols, cursorHere, starts);
}

int VimLayout::rowsOf(const Vim& vim, int line, int cols) const {
    std::vector<int> s;
    segments(vim, line, cols, s);
    return (int)s.size();
}

// Index of the row (segment) of `line` that holds column `col`.
static int segmentOf(const std::vector<int>& starts, int col) {
    int i = (int)starts.size() - 1;
    while (i > 0 && starts[i] > col) i--;
    return i;
}

const std::vector<VimRow>& VimLayout::update(const Vim& vim, int cols, int rows) {
    rows_.assign(std::max(0, rows), VimRow());
    if (cols <= 0 || rows <= 0) return rows_;
    const auto& lines = vim.lines();
    const int nlines = (int)lines.size();
    const int curLine = vim.row();
    std::vector<int> segs;
    segments(vim, curLine, cols, segs);
    const int curSub = segmentOf(segs, vim.col());

    if (top_ >= nlines) {
        top_ = nlines - 1;
        topSub_ = 0;
    }
    if (top_ < 0) top_ = 0;
    topSub_ = std::max(0, std::min(topSub_, rowsOf(vim, top_, cols) - 1));

    if (curLine < top_ || (curLine == top_ && curSub < topSub_)) {
        top_ = curLine;
        topSub_ = curSub;
    } else {
        // screen rows between the top of the view and the cursor row
        long dist = curSub - topSub_;
        for (int l = top_; l < curLine && dist < rows; l++) dist += rowsOf(vim, l, cols);
        if (dist >= rows) {
            // the cursor is below the view: put it on the last row by walking back from it
            int line = curLine, sub = curSub;
            for (int k = 0; k < rows - 1; k++) {
                if (sub > 0) {
                    sub--;
                } else if (line > 0) {
                    line--;
                    sub = rowsOf(vim, line, cols) - 1;
                } else {
                    break;
                }
            }
            top_ = line;
            topSub_ = sub;
        }
    }

    int line = top_, sub = topSub_;
    segments(vim, line, cols, segs);
    for (int y = 0; y < rows; y++) {
        VimRow& r = rows_[y];
        if (line >= nlines) continue;  // past the end
        const int n = (int)lines[line].size();
        r.line = line;
        r.start = segs[sub];
        r.len = (sub + 1 < (int)segs.size() ? segs[sub + 1] : n) - r.start;
        if (r.len < 0) r.len = 0;
        if (line == curLine && sub == curSub) r.cursor = vim.col() - r.start;
        if (++sub >= (int)segs.size()) {
            sub = 0;
            line++;
            if (line < nlines) segments(vim, line, cols, segs);
        }
    }
    return rows_;
}
