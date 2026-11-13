#include "merge.h"

namespace merge {

std::string label(uint8_t e) {
    if (e == 0) return std::string();
    if (e < 32) return std::to_string(1UL << e);
    return "?";
}

namespace {
// Index of the i-th cell of line `line`, counted from the wall the tiles move towards.
int cellOf(Dir d, int line, int i) {
    switch (d) {
        case Dir::Left: return line * kSize + i;
        case Dir::Right: return line * kSize + (kSize - 1 - i);
        case Dir::Up: return i * kSize + line;
        default: return (kSize - 1 - i) * kSize + line;
    }
}
}  // namespace

uint32_t slide(Board& b, Dir d, bool& moved) {
    moved = false;
    uint32_t gained = 0;
    for (int line = 0; line < kSize; line++) {
        uint8_t in[kSize], out[kSize] = {0, 0, 0, 0};
        int n = 0;
        for (int i = 0; i < kSize; i++) {
            const uint8_t v = b[cellOf(d, line, i)];
            if (v) in[n++] = v;
        }
        int o = 0;
        for (int i = 0; i < n; i++) {
            if (i + 1 < n && in[i] == in[i + 1] && in[i] < 31) {
                out[o++] = in[i] + 1;
                gained += 1UL << (in[i] + 1);
                i++;
            } else {
                out[o++] = in[i];
            }
        }
        for (int i = 0; i < kSize; i++) {
            uint8_t& cell = b[cellOf(d, line, i)];
            if (cell != out[i]) moved = true;
            cell = out[i];
        }
    }
    return gained;
}

bool canMove(const Board& b) {
    for (int r = 0; r < kSize; r++)
        for (int c = 0; c < kSize; c++) {
            const uint8_t v = b[r * kSize + c];
            if (!v) return true;
            if (c + 1 < kSize && b[r * kSize + c + 1] == v) return true;
            if (r + 1 < kSize && b[(r + 1) * kSize + c] == v) return true;
        }
    return false;
}

uint8_t highest(const Board& b) {
    uint8_t h = 0;
    for (uint8_t v : b)
        if (v > h) h = v;
    return h;
}

void Game::reset() {
    board_.fill(0);
    score_ = 0;
    hasUndo_ = false;
    spawn();
    spawn();
    lastSpawn_ = -1;
}

void Game::spawn() {
    int empty[kCells], n = 0;
    for (int i = 0; i < kCells; i++)
        if (!board_[i]) empty[n++] = i;
    if (!n) {
        lastSpawn_ = -1;
        return;
    }
    lastSpawn_ = empty[rng_.below(n)];
    board_[lastSpawn_] = rng_.below(10) == 0 ? 2 : 1;  // a 4 one time in ten
}

bool Game::move(Dir d) {
    Board next = board_;
    bool moved;
    const uint32_t gained = slide(next, d, moved);
    if (!moved) return false;
    prevBoard_ = board_;
    prevScore_ = score_;
    hasUndo_ = true;
    board_ = next;
    score_ += gained;
    spawn();
    return true;
}

bool Game::undo() {
    if (!hasUndo_) return false;
    board_ = prevBoard_;
    score_ = prevScore_;
    hasUndo_ = false;
    lastSpawn_ = -1;
    return true;
}

std::string Game::serialize() const {
    static const char hex[] = "0123456789abcdefghijklmnopqrstuv";
    std::string s;
    for (uint8_t v : board_) s.push_back(hex[v & 31]);
    return s + " " + std::to_string(score_);
}

bool Game::load(const std::string& text) {
    if (text.size() < kCells + 2 || text[kCells] != ' ') return false;
    Board b;
    for (int i = 0; i < kCells; i++) {
        const char c = text[i];
        if (c >= '0' && c <= '9') b[i] = c - '0';
        else if (c >= 'a' && c <= 'v') b[i] = c - 'a' + 10;
        else return false;
    }
    uint32_t score = 0;
    for (size_t i = kCells + 1; i < text.size(); i++) {
        if (text[i] < '0' || text[i] > '9') return false;
        score = score * 10 + (text[i] - '0');
    }
    board_ = b;
    score_ = score;
    hasUndo_ = false;
    lastSpawn_ = -1;
    return true;
}

}  // namespace merge
