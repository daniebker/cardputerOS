// Merge: a sliding tile puzzle on a 4x4 board. Every move slides all tiles in one
// direction; two equal tiles that meet become one tile of twice the value. After
// each move a new 2 (or sometimes a 4) appears on an empty square. The goal is a
// 2048 tile; the game ends when no move is possible.
//
// Portable, no platform dependencies. Tiles are stored as exponents: 0 = empty,
// 1 = 2, 2 = 4, ... 11 = 2048.
#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace merge {

constexpr int kSize = 4;
constexpr int kCells = kSize * kSize;
constexpr uint8_t kGoal = 11;  // 2^11 = 2048

enum class Dir : uint8_t { Left, Right, Up, Down };

using Board = std::array<uint8_t, kCells>;  // row-major

// Small deterministic PRNG (xorshift32).
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed = 1) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int below(int n) { return (int)(next() % (uint32_t)n); }
};

// Value shown on a tile ("" for an empty square).
std::string label(uint8_t exponent);

// Slides `b` in direction `d`. Returns the points gained (the sum of the new
// merged tiles) and sets `moved` if anything changed. Each tile merges at most
// once per move, and the tiles nearest the wall merge first.
uint32_t slide(Board& b, Dir d, bool& moved);

bool canMove(const Board& b);
uint8_t highest(const Board& b);

class Game {
public:
    explicit Game(uint32_t seed = 1) : rng_(seed) { reset(); }

    void reset();  // a new game: an empty board with two tiles
    // Makes a move; false if nothing moved (then nothing changes, no tile appears).
    bool move(Dir d);
    bool undo();  // takes back the last move (one step)

    const Board& board() const { return board_; }
    uint32_t score() const { return score_; }
    bool over() const { return !canMove(board_); }
    bool won() const { return highest(board_) >= kGoal; }
    bool canUndo() const { return hasUndo_; }
    int lastSpawn() const { return lastSpawn_; }  // cell of the tile added by the last move, -1 = none

    // Saved game: "board-hex score" (e.g. "1100000000000000 0"). load() returns false if invalid.
    std::string serialize() const;
    bool load(const std::string& text);

private:
    void spawn();

    Rng rng_;
    Board board_{}, prevBoard_{};
    uint32_t score_ = 0, prevScore_ = 0;
    bool hasUndo_ = false;
    int lastSpawn_ = -1;
};

}  // namespace merge
