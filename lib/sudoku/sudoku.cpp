#include "sudoku.h"

namespace sudoku {

namespace {

constexpr uint16_t kAll = 0x3FE;  // bits 1..9

int popcount(uint16_t v) {
    int n = 0;
    while (v) {
        v &= v - 1;
        n++;
    }
    return n;
}

// Bitmask solver state: digits used per row / column / box.
struct State {
    Grid g;
    uint16_t rows[9], cols[9], boxes[9];

    bool init(const Grid& in) {
        g = in;
        for (int k = 0; k < 9; k++) rows[k] = cols[k] = boxes[k] = 0;
        for (int i = 0; i < 81; i++) {
            if (!g[i]) continue;
            const uint16_t b = 1u << g[i];
            if ((rows[row(i)] | cols[col(i)] | boxes[box(i)]) & b) return false;  // invalid givens
            rows[row(i)] |= b;
            cols[col(i)] |= b;
            boxes[box(i)] |= b;
        }
        return true;
    }
    uint16_t free(int i) const { return kAll & ~(rows[row(i)] | cols[col(i)] | boxes[box(i)]); }
    void set(int i, int d) {
        g[i] = (uint8_t)d;
        const uint16_t b = 1u << d;
        rows[row(i)] |= b;
        cols[col(i)] |= b;
        boxes[box(i)] |= b;
    }
    void clear(int i) {
        const uint16_t b = 1u << g[i];
        rows[row(i)] &= ~b;
        cols[col(i)] &= ~b;
        boxes[box(i)] &= ~b;
        g[i] = 0;
    }
};

// Depth-first search choosing the most constrained empty cell first.
// With `rng`, digits are tried in random order (used to build random full grids).
void search(State& st, int limit, int& found, Grid* first, Rng* rng) {
    int best = -1, bestCount = 10;
    uint16_t bestMask = 0;
    for (int i = 0; i < 81; i++) {
        if (st.g[i]) continue;
        const uint16_t m = st.free(i);
        const int c = popcount(m);
        if (c == 0) return;  // dead end
        if (c < bestCount) {
            best = i;
            bestCount = c;
            bestMask = m;
            if (c == 1) break;
        }
    }
    if (best < 0) {  // grid full: a solution
        if (found == 0 && first) *first = st.g;
        found++;
        return;
    }
    int digits[9], n = 0;
    for (int d = 1; d <= 9; d++)
        if (bestMask & (1u << d)) digits[n++] = d;
    if (rng) {
        for (int k = n - 1; k > 0; k--) {
            const int j = rng->below(k + 1);
            const int t = digits[k];
            digits[k] = digits[j];
            digits[j] = t;
        }
    }
    for (int k = 0; k < n && found < limit; k++) {
        st.set(best, digits[k]);
        search(st, limit, found, first, rng);
        st.clear(best);
    }
}

}  // namespace

int countSolutions(const Grid& g, int limit, Grid* solution) {
    State st;
    if (!st.init(g)) return 0;
    int found = 0;
    search(st, limit, found, solution, nullptr);
    return found;
}

bool solve(const Grid& g, Grid& solution) { return countSolutions(g, 1, &solution) == 1; }

void generate(Difficulty d, uint32_t seed, Grid& puzzle, Grid& solution) {
    Rng rng(seed);
    // 1. a random complete grid
    State st;
    Grid empty{};
    st.init(empty);
    int found = 0;
    search(st, 1, found, &solution, &rng);

    // 2. remove cells (in symmetric pairs, which looks nicer) while the solution stays unique
    const int target = d == Difficulty::Easy ? 40 : (d == Difficulty::Medium ? 32 : 17);
    puzzle = solution;
    int order[41];
    for (int i = 0; i <= 40; i++) order[i] = i;  // cell i and its mirror 80 - i
    for (int k = 40; k > 0; k--) {
        const int j = rng.below(k + 1);
        const int t = order[k];
        order[k] = order[j];
        order[j] = t;
    }
    int givens = 81;
    for (int k = 0; k <= 40 && givens > target; k++) {
        const int a = order[k], b = 80 - a;
        const uint8_t va = puzzle[a], vb = puzzle[b];
        puzzle[a] = puzzle[b] = 0;
        const int removed = (a == b) ? 1 : 2;
        if (givens - removed < target - 1 || countSolutions(puzzle, 2) != 1) {
            puzzle[a] = va;  // would make it ambiguous (or too sparse): keep this pair
            puzzle[b] = vb;
        } else {
            givens -= removed;
        }
    }
}

bool conflicts(const Grid& g, int i) {
    const uint8_t v = g[i];
    if (!v) return false;
    for (int j = 0; j < 81; j++)
        if (j != i && g[j] == v && sees(i, j)) return true;
    return false;
}

uint16_t candidates(const Grid& g, int i) {
    uint16_t used = 0;
    for (int j = 0; j < 81; j++)
        if (j != i && g[j] && sees(i, j)) used |= 1u << g[j];
    return kAll & ~used;
}

}  // namespace sudoku
