#include <unity.h>

#include <chrono>
#include <cstdio>

#include "sudoku.h"

using namespace sudoku;

void setUp() {}
void tearDown() {}

static Grid parse(const char* s) {
    Grid g{};
    for (int i = 0; i < 81; i++) g[i] = (s[i] >= '1' && s[i] <= '9') ? s[i] - '0' : 0;
    return g;
}

static bool validComplete(const Grid& g) {
    for (int i = 0; i < 81; i++)
        if (g[i] < 1 || g[i] > 9 || conflicts(g, i)) return false;
    return true;
}

void test_solve_known_puzzle() {
    // a well-known hard puzzle ("Arto Inkala")
    Grid g = parse("8..........36......7..9.2...5...7.......457.....1...3...1....68..85...1..9....4..");
    Grid sol;
    TEST_ASSERT_TRUE(solve(g, sol));
    TEST_ASSERT_TRUE(validComplete(sol));
    for (int i = 0; i < 81; i++)
        if (g[i]) TEST_ASSERT_EQUAL(g[i], sol[i]);
    TEST_ASSERT_EQUAL(1, countSolutions(g, 2));
    TEST_ASSERT_EQUAL_UINT8(8, sol[0]);
    TEST_ASSERT_EQUAL_UINT8(1, sol[1]);
    TEST_ASSERT_EQUAL_UINT8(2, sol[2]);
}

void test_counts_and_invalid() {
    Grid empty{};
    TEST_ASSERT_EQUAL(2, countSolutions(empty, 2));
    Grid bad{};
    bad[0] = 5;
    bad[1] = 5;  // same row
    TEST_ASSERT_EQUAL(0, countSolutions(bad, 2));
    Grid sol;
    TEST_ASSERT_FALSE(solve(bad, sol));
}

void test_rules() {
    Grid g{};
    g[0] = 3;
    g[80] = 3;
    TEST_ASSERT_FALSE(conflicts(g, 0));
    g[10] = 3;  // same box as cell 0
    TEST_ASSERT_TRUE(conflicts(g, 0));
    TEST_ASSERT_TRUE(conflicts(g, 10));
    TEST_ASSERT_FALSE(conflicts(g, 80));
    g[10] = 0;
    g[8] = 7;  // row 0
    g[72] = 9;  // column 0
    const uint16_t c = candidates(g, 0);
    TEST_ASSERT_TRUE(c & (1 << 3));  // its own value is not counted against it
    TEST_ASSERT_FALSE(c & (1 << 7));
    TEST_ASSERT_FALSE(c & (1 << 9));
    TEST_ASSERT_TRUE(c & (1 << 1));
    TEST_ASSERT_TRUE(sees(0, 8));
    TEST_ASSERT_TRUE(sees(0, 20));
    TEST_ASSERT_FALSE(sees(0, 30));
}

void test_generate_unique_and_consistent() {
    for (int di = 0; di < 3; di++) {
        int minGivens = 81, maxGivens = 0;
        long long totalUs = 0, maxUs = 0;
        for (uint32_t seed = 1; seed <= 30; seed++) {
            Grid p, s;
            auto t0 = std::chrono::steady_clock::now();
            generate((Difficulty)di, seed * 7919u, p, s);
            long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
            totalUs += us;
            if (us > maxUs) maxUs = us;
            TEST_ASSERT_TRUE(validComplete(s));
            int givens = 0;
            for (int i = 0; i < 81; i++) {
                if (p[i]) {
                    givens++;
                    TEST_ASSERT_EQUAL(s[i], p[i]);
                    TEST_ASSERT_EQUAL(s[80 - i], p[80 - i]);
                    TEST_ASSERT_TRUE(p[80 - i] != 0);  // symmetric
                }
            }
            Grid other;
            TEST_ASSERT_EQUAL(1, countSolutions(p, 2, &other));
            TEST_ASSERT_TRUE(other == s);
            if (givens < minGivens) minGivens = givens;
            if (givens > maxGivens) maxGivens = givens;
        }
        printf("difficulty %d: givens %d..%d, avg %lld us, max %lld us\n", di, minGivens, maxGivens, totalUs / 30, maxUs);
        if (di == 0) TEST_ASSERT_TRUE(minGivens >= 39 && maxGivens <= 41);
        if (di == 1) TEST_ASSERT_TRUE(minGivens >= 31 && maxGivens <= 33);
        if (di == 2) TEST_ASSERT_TRUE(maxGivens <= 32);
    }
    // same seed, same puzzle
    Grid a, b, sa, sb;
    generate(Difficulty::Medium, 42, a, sa);
    generate(Difficulty::Medium, 42, b, sb);
    TEST_ASSERT_TRUE(a == b);
    generate(Difficulty::Medium, 43, b, sb);
    TEST_ASSERT_FALSE(a == b);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_solve_known_puzzle);
    RUN_TEST(test_counts_and_invalid);
    RUN_TEST(test_rules);
    RUN_TEST(test_generate_unique_and_consistent);
    return UNITY_END();
}
