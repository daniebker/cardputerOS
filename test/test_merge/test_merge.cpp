#include <unity.h>

#include "merge.h"

using namespace merge;

void setUp() {}
void tearDown() {}

static Board row(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    Board x{};
    x[0] = a, x[1] = b, x[2] = c, x[3] = d;
    return x;
}

static void expectRow(const Board& b, uint8_t a, uint8_t c1, uint8_t c2, uint8_t d) {
    TEST_ASSERT_EQUAL(a, b[0]);
    TEST_ASSERT_EQUAL(c1, b[1]);
    TEST_ASSERT_EQUAL(c2, b[2]);
    TEST_ASSERT_EQUAL(d, b[3]);
}

void test_slide_rules() {
    bool moved;
    Board b = row(1, 1, 1, 1);  // 2 2 2 2 -> 4 4
    TEST_ASSERT_EQUAL(8, slide(b, Dir::Left, moved));
    TEST_ASSERT_TRUE(moved);
    expectRow(b, 2, 2, 0, 0);

    b = row(1, 0, 1, 2);  // 2 . 2 4 -> 4 4 (a merged tile does not merge again)
    TEST_ASSERT_EQUAL(4, slide(b, Dir::Left, moved));
    expectRow(b, 2, 2, 0, 0);

    b = row(1, 1, 1, 0);  // 2 2 2 . right -> . . 2 4 (nearest the wall first)
    TEST_ASSERT_EQUAL(4, slide(b, Dir::Right, moved));
    expectRow(b, 0, 0, 1, 2);

    b = row(1, 2, 3, 4);  // nothing to do
    TEST_ASSERT_EQUAL(0, slide(b, Dir::Left, moved));
    TEST_ASSERT_FALSE(moved);
    slide(b, Dir::Right, moved);
    TEST_ASSERT_FALSE(moved);

    // columns
    Board c{};
    c[1] = 3, c[5] = 3, c[13] = 1;  // column 1: 8 8 . 2
    TEST_ASSERT_EQUAL(16, slide(c, Dir::Down, moved));
    TEST_ASSERT_EQUAL(1, c[13]);  // the 2 is already at the wall
    TEST_ASSERT_EQUAL(4, c[9]);   // 8 + 8 above it
    TEST_ASSERT_EQUAL(0, c[5]);
    TEST_ASSERT_EQUAL(0, c[1]);
}

void test_can_move() {
    Board b{};
    for (int i = 0; i < kCells; i++) b[i] = (uint8_t)(1 + (i + i / kSize) % 2);  // checkerboard 2/4
    TEST_ASSERT_FALSE(canMove(b));
    b[5] = b[6];
    TEST_ASSERT_TRUE(canMove(b));
    b[5] = 0;
    TEST_ASSERT_TRUE(canMove(b));
}

void test_game_and_undo() {
    Game g(42);
    int tiles = 0;
    for (uint8_t v : g.board()) tiles += v ? 1 : 0;
    TEST_ASSERT_EQUAL(2, tiles);
    TEST_ASSERT_FALSE(g.canUndo());

    // play randomly until the game ends: score and tiles stay consistent
    Rng r(7);
    int moves = 0;
    while (!g.over() && moves < 100000) {
        const Board before = g.board();
        const uint32_t score = g.score();
        if (g.move((Dir)r.below(4))) {
            moves++;
            TEST_ASSERT_TRUE(g.lastSpawn() >= 0);
            TEST_ASSERT_TRUE(g.score() >= score);
            if (moves % 50 == 0) {
                TEST_ASSERT_TRUE(g.undo());
                TEST_ASSERT_TRUE(before == g.board());
                TEST_ASSERT_EQUAL(score, g.score());
                TEST_ASSERT_FALSE(g.undo());
            }
        } else {
            TEST_ASSERT_TRUE(before == g.board());
        }
    }
    TEST_ASSERT_TRUE(g.over());
    TEST_ASSERT_TRUE(moves > 20);
    // the score is the sum of all merged tiles, so it is at least the largest tile
    TEST_ASSERT_TRUE(g.score() >= (1UL << highest(g.board())) || highest(g.board()) <= 2);
}

void test_save_load() {
    Game g(3);
    for (int i = 0; i < 30; i++) g.move((Dir)(i % 4));
    Game h(9);
    TEST_ASSERT_TRUE(h.load(g.serialize()));
    TEST_ASSERT_TRUE(g.board() == h.board());
    TEST_ASSERT_EQUAL(g.score(), h.score());
    TEST_ASSERT_FALSE(h.load(""));
    TEST_ASSERT_FALSE(h.load("00000000000000000"));
    TEST_ASSERT_FALSE(h.load("000000000000000z 5"));
    TEST_ASSERT_FALSE(h.load("0000000000000000 x"));
    TEST_ASSERT_TRUE(h.load("b100000000000000 20480"));
    TEST_ASSERT_TRUE(h.won());
    TEST_ASSERT_EQUAL_STRING("2048", label(h.board()[0]).c_str());
    TEST_ASSERT_EQUAL_STRING("", label(0).c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_slide_rules);
    RUN_TEST(test_can_move);
    RUN_TEST(test_game_and_undo);
    RUN_TEST(test_save_load);
    return UNITY_END();
}
