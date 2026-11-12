#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <queue>
#include <string>
#include <vector>

#include "rogue.h"

using namespace rogue;

void setUp() {}
void tearDown() {}

// Shortest path over cells the bot knows (explored and walkable). Returns the first
// step towards the nearest cell for which `goal` is true, or false if none is reachable.
template <typename F>
static bool pathTo(const Game& g, F goal, int& dx, int& dy, bool throughUnknown = false) {
    const Player& p = g.player();
    std::vector<int> prev(W * H, -1);
    std::queue<int> q;
    const int start = p.y * W + p.x;
    prev[start] = start;
    q.push(start);
    while (!q.empty()) {
        const int c = q.front();
        q.pop();
        const int cx = c % W, cy = c / W;
        if (c != start && goal(cx, cy)) {
            int s = c;
            while (prev[s] != start) s = prev[s];
            dx = s % W - p.x;
            dy = s / W - p.y;
            return true;
        }
        for (int oy = -1; oy <= 1; oy++)
            for (int ox = -1; ox <= 1; ox++) {
                const int nx = cx + ox, ny = cy + oy;
                if (!Game::inside(nx, ny) || prev[ny * W + nx] >= 0) continue;
                const Tile t = g.tile(nx, ny);
                if (t != Floor && t != Stairs) continue;
                if (!throughUnknown && !g.explored(nx, ny)) continue;
                prev[ny * W + nx] = c;
                q.push(ny * W + nx);
            }
    }
    return false;
}

// A simple but sensible player: heals when hurt, uses better gear, fights what is
// adjacent, collects items, explores, then takes the stairs.
static void botTurn(Game& g) {
    const Player& p = g.player();
    const auto& inv = g.inventory();
    for (int i = 0; i < (int)inv.size(); i++) {
        if (inv[i].kind == PotionHeal && p.hp * 100 < p.maxHp * 40) { g.use(i); return; }
        if (inv[i].kind == PotionMight) { g.use(i); return; }
        if (inv[i].kind == Weapon && inv[i].amount > p.weapon) { g.use(i); return; }
        if (inv[i].kind == Armor && inv[i].amount > p.armor) { g.use(i); return; }
    }
    for (auto& m : g.monsters())
        if (std::max(abs(m.x - p.x), abs(m.y - p.y)) == 1) { g.move(m.x - p.x, m.y - p.y); return; }
    // wait for a monster that is coming, with our back to nothing in particular
    static int waited = 0;  // do not wait forever for a monster that cannot reach us
    for (auto& m : g.monsters())
        if (waited < 6 && g.visible(m.x, m.y) && m.awake && std::max(abs(m.x - p.x), abs(m.y - p.y)) <= 3) { waited++; g.rest(); return; }
    if (waited >= 6 && ++waited > 30) waited = 0;
    if (const Item* it = g.itemAt(p.x, p.y)) {
        if (it->kind == Crown || (int)inv.size() < kMaxInventory) { g.pickUp(); return; }
    }
    // rest up before moving on
    if (p.hp * 100 < p.maxHp * 70) { g.rest(); return; }
    int dx, dy;
    const bool room = (int)inv.size() < kMaxInventory;
    if (pathTo(g, [&](int x, int y) { const Item* it = g.itemAt(x, y); return it && g.explored(x, y) && (room || it->kind == Crown || it->kind == Gold); }, dx, dy)) { g.move(dx, dy); return; }
    // frontier: an explored walkable cell next to an unexplored one
    if (pathTo(g, [&](int x, int y) {
            for (int oy = -1; oy <= 1; oy++)
                for (int ox = -1; ox <= 1; ox++)
                    if (Game::inside(x + ox, y + oy) && !g.explored(x + ox, y + oy)) {
                        const Tile t = g.tile(x + ox, y + oy);
                        if (t == Floor || t == Stairs) return true;
                    }
            return false; }, dx, dy)) { g.move(dx, dy); return; }
    if (g.tile(p.x, p.y) == Stairs) { g.descend(); return; }
    if (pathTo(g, [&](int x, int y) { return g.tile(x, y) == Stairs; }, dx, dy)) { g.move(dx, dy); return; }
    g.rest();
}

void test_levels_are_connected() {
    for (uint32_t seed = 1; seed <= 300; seed++) {
        Game g;
        g.newGame(seed * 2654435761u);
        // every walkable cell, the stairs and all items must be reachable from the start
        int dx, dy;
        int floor = 0, stairs = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const Tile t = g.tile(x, y);
                if (t == Stairs) stairs++;
                if (t == Floor || t == Stairs) {
                    floor++;
                    if (x != g.player().x || y != g.player().y)
                        TEST_ASSERT_TRUE(pathTo(g, [&](int cx, int cy) { return cx == x && cy == y; }, dx, dy, true));
                    // walkable cells are never on the border and always enclosed
                    TEST_ASSERT_TRUE(x > 0 && y > 0 && x < W - 1 && y < H - 1);
                }
            }
        TEST_ASSERT_EQUAL(1, stairs);
        TEST_ASSERT_TRUE(floor > 60);
        TEST_ASSERT_TRUE(g.tile(g.player().x, g.player().y) == Floor);
        for (auto& m : g.monsters()) TEST_ASSERT_TRUE(g.tile(m.x, m.y) == Floor);
    }
}

void test_fov_and_memory() {
    Game g;
    g.newGame(1234);
    const Player& p = g.player();
    TEST_ASSERT_TRUE(g.visible(p.x, p.y));
    int vis = 0, expl = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            if (g.visible(x, y)) {
                vis++;
                TEST_ASSERT_TRUE(g.explored(x, y));
            }
            if (g.explored(x, y)) expl++;
        }
    TEST_ASSERT_TRUE(vis >= 12);
    TEST_ASSERT_TRUE(vis < W * H / 2);  // walls block the view
    TEST_ASSERT_EQUAL(vis, expl);
}

void test_actions() {
    Game g;
    g.newGame(77);
    g.takeMessages();
    TEST_ASSERT_EQUAL(1, (int)g.inventory().size());
    TEST_ASSERT_FALSE(g.descend());  // not on stairs
    TEST_ASSERT_FALSE(g.pickUp());   // nothing here
    TEST_ASSERT_FALSE(g.move(0, 0));
    TEST_ASSERT_FALSE(g.use(5));
    const int turns = g.player().turns;
    TEST_ASSERT_TRUE(g.rest());
    TEST_ASSERT_EQUAL(turns + 1, g.player().turns);
    TEST_ASSERT_TRUE(g.drop(0));
    TEST_ASSERT_EQUAL(0, (int)g.inventory().size());
    TEST_ASSERT_TRUE(g.itemAt(g.player().x, g.player().y) != nullptr);
    TEST_ASSERT_TRUE(g.pickUp());
    TEST_ASSERT_EQUAL(1, (int)g.inventory().size());
    TEST_ASSERT_TRUE(g.use(0));  // drink
    TEST_ASSERT_EQUAL(0, (int)g.inventory().size());
    TEST_ASSERT_FALSE(g.takeMessages().empty());
    TEST_ASSERT_EQUAL_STRING("long sword +4", itemName(Weapon, 4).c_str());
    TEST_ASSERT_EQUAL_STRING("35 gold", itemName(Gold, 35).c_str());
}

void test_save_load_roundtrip() {
    Game g;
    g.newGame(4242);
    for (int i = 0; i < 300 && g.state() == State::Playing; i++) botTurn(g);
    const std::string s = g.save();
    Game h;
    TEST_ASSERT_TRUE(h.load(s));
    TEST_ASSERT_TRUE(h.save() == s);
    // both copies play on identically (the random state is saved too)
    for (int i = 0; i < 200 && g.state() == State::Playing; i++) {
        botTurn(g);
        botTurn(h);
    }
    TEST_ASSERT_TRUE(g.save() == h.save());
    // corrupted / truncated saves are rejected, never crash
    Game k;
    TEST_ASSERT_FALSE(k.load(""));
    TEST_ASSERT_FALSE(k.load(s.substr(0, s.size() / 2)));
    TEST_ASSERT_FALSE(k.load(s + "x"));
    uint32_t r = 5;
    for (int i = 0; i < 3000; i++) {
        std::string bad = s;
        r = r * 1664525u + 1013904223u;
        bad[(r >> 8) % bad.size()] = (char)(r >> 24);
        Game b;
        if (b.load(bad)) {
            for (int t = 0; t < 30; t++) botTurn(b);  // whatever loads must be playable
        }
    }
}

void test_bot_plays_many_games() {
    int wins = 0, deaths = 0, stuck = 0;
    long depthSum = 0;
    int depthHist[kMaxDepth + 1] = {0};
    const int games = 200;
    for (uint32_t seed = 1; seed <= (uint32_t)games; seed++) {
        Game g;
        g.newGame(seed * 7919u + 13);
        int turns = 0;
        while (g.state() == State::Playing && turns < 20000) {
            botTurn(g);
            turns++;
            const Player& p = g.player();
            TEST_ASSERT_TRUE(p.hp <= p.maxHp && (p.hp > 0 || g.state() == State::Dead));
            TEST_ASSERT_TRUE(g.tile(p.x, p.y) != Wall && g.tile(p.x, p.y) != Rock);
            TEST_ASSERT_TRUE((int)g.inventory().size() <= kMaxInventory + 1);
            g.takeMessages();
        }
        if (g.state() == State::Won) wins++;
        else if (g.state() == State::Dead) {
            deaths++;
            TEST_ASSERT_FALSE(g.causeOfDeath().empty());
        } else {
            stuck++;
            const Player& p = g.player();
            printf("stuck seed %u: depth %d pos %d,%d hp %d/%d inv %d\n", seed, p.depth, p.x, p.y, p.hp, p.maxHp, (int)g.inventory().size());
            for (auto& m : g.monsters())
                if (g.visible(m.x, m.y)) printf("   visible %s at %d,%d awake=%d\n", monsterInfo(m.kind).name, m.x, m.y, m.awake);
            if (const Item* it = g.itemAt(p.x, p.y)) printf("   standing on %s\n", itemName(it->kind, it->amount).c_str());
        }
        depthSum += g.player().depth;
        depthHist[g.player().depth]++;
    }
    printf("bot: %d games, %d wins, %d deaths, %d unfinished, average depth %.1f\n", games, wins, deaths, stuck,
           (double)depthSum / games);
    printf("bot: final depth histogram:");
    for (int d = 1; d <= kMaxDepth; d++) printf(" d%d=%d", d, depthHist[d]);
    printf("\n");
    TEST_ASSERT_EQUAL(0, stuck);
    // winnable, but not a walkover, for a careful but simple player
    TEST_ASSERT_TRUE(wins >= games * 5 / 100);
    TEST_ASSERT_TRUE(wins <= games * 45 / 100);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_levels_are_connected);
    RUN_TEST(test_fov_and_memory);
    RUN_TEST(test_actions);
    RUN_TEST(test_save_load_roundtrip);
    RUN_TEST(test_bot_plays_many_games);
    return UNITY_END();
}
