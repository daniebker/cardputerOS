// Sudoku engine: puzzle generation (unique solution), solving, rule checks.
// Portable, no platform dependencies. Grids are 81 cells, row-major, 0 = empty.
#pragma once
#include <array>
#include <cstdint>

namespace sudoku {

using Grid = std::array<uint8_t, 81>;

enum class Difficulty : uint8_t { Easy = 0, Medium = 1, Hard = 2 };

// Small deterministic PRNG (xorshift32) so puzzles are reproducible from a seed.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int below(int n) { return (int)(next() % (uint32_t)n); }
};

// Counts solutions of `g` up to `limit` (stops early). `solution` receives the
// first solution found, if non-null.
int countSolutions(const Grid& g, int limit, Grid* solution = nullptr);

// Solves `g`; returns false if it has no solution.
bool solve(const Grid& g, Grid& solution);

// Makes a puzzle with exactly one solution. Easy keeps about 40 givens,
// Medium about 32, Hard as few as it can (typically 24-28).
void generate(Difficulty d, uint32_t seed, Grid& puzzle, Grid& solution);

// True if the value in cell i repeats in its row, column or box.
bool conflicts(const Grid& g, int i);

// Digits allowed in cell i by the rules (bit d set = digit d possible, bits 1..9).
uint16_t candidates(const Grid& g, int i);

inline int row(int i) { return i / 9; }
inline int col(int i) { return i % 9; }
inline int box(int i) { return (i / 27) * 3 + (i % 9) / 3; }
// True if cells a and b share a row, column or box.
inline bool sees(int a, int b) { return row(a) == row(b) || col(a) == col(b) || box(a) == box(b); }

}  // namespace sudoku
