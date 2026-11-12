// A small original roguelike: random dungeons, field of view, monsters, items,
// permadeath. Portable game rules only; drawing and input live in the app.
// Goal: descend to depth 8 and take the Ember Crown.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rogue {

constexpr int W = 40, H = 21;
constexpr int kMaxDepth = 8;
constexpr int kMaxInventory = 12;

enum Tile : uint8_t { Rock = 0, Wall, Floor, Stairs };
enum class State : uint8_t { Playing, Dead, Won };

enum ItemKind : uint8_t { Gold = 0, PotionHeal, PotionMight, ScrollTeleport, ScrollMap, Weapon, Armor, Crown, kItemKinds };

struct Monster {
    uint8_t kind = 0;
    int8_t x = 0, y = 0;
    int16_t hp = 0;
    bool awake = false;
};

struct Item {
    uint8_t kind = 0;
    int8_t x = 0, y = 0;
    int16_t amount = 0;  // gold: coins; weapon / armor: bonus
};

struct InvItem {
    uint8_t kind = 0;
    int16_t amount = 0;
};

struct Player {
    int x = 0, y = 0;
    int hp = 0, maxHp = 0;
    int level = 1, xp = 0, gold = 0;
    int weapon = 0, armor = 0;  // bonus of the equipped gear (0 = none)
    int might = 0;              // permanent attack bonus from potions
    int depth = 1;
    int turns = 0;
};

struct MonsterInfo {
    char glyph;
    const char* name;
    int hp, atk, def, xp, minDepth;
    bool erratic;
};
const MonsterInfo& monsterInfo(int kind);
int monsterKinds();
char itemGlyph(int kind);
std::string itemName(int kind, int amount);

class Game {
public:
    void newGame(uint32_t seed);
    State state() const { return state_; }

    // Actions. Each returns true if it used a turn (monsters then act).
    bool move(int dx, int dy);  // also attacks a monster in the way
    bool rest();
    bool pickUp();
    bool descend();
    bool use(int index);   // drink / read / equip
    bool drop(int index);

    Tile tile(int x, int y) const { return inside(x, y) ? (Tile)tiles_[y * W + x] : Rock; }
    bool visible(int x, int y) const { return inside(x, y) && (flags_[y * W + x] & 1); }
    bool explored(int x, int y) const { return inside(x, y) && (flags_[y * W + x] & 2); }
    const Monster* monsterAt(int x, int y) const;
    const Item* itemAt(int x, int y) const;
    const std::vector<Monster>& monsters() const { return monsters_; }
    const std::vector<Item>& items() const { return items_; }
    const Player& player() const { return p_; }
    const std::vector<InvItem>& inventory() const { return inv_; }
    int attack() const { return 3 + p_.level + p_.weapon + p_.might; }
    int defense() const { return p_.armor; }
    int xpForNextLevel() const { return 10 * p_.level * (p_.level + 1) / 2; }
    const std::string& causeOfDeath() const { return killer_; }

    // Messages produced since the last call (oldest first).
    std::vector<std::string> takeMessages();

    // Whole game state as bytes, for saving. load() returns false on bad data.
    std::string save() const;
    bool load(const std::string& data);

    static bool inside(int x, int y) { return x >= 0 && x < W && y >= 0 && y < H; }

private:
    uint32_t rnd();
    int rnd(int lo, int hi);  // inclusive
    void say(const std::string& m) { messages_.push_back(m); }
    void makeLevel();
    int pickMonsterKind();
    void updateFov();
    void endTurn();
    void monstersAct();
    void attackMonster(Monster& m);
    void monsterAttacks(const Monster& m);
    void gainXp(int xp);
    bool walkable(int x, int y) const { return tile(x, y) == Floor || tile(x, y) == Stairs; }
    bool freeCell(int x, int y) const;
    bool randomFloor(int& x, int& y, int avoidX, int avoidY, int minDist);

    uint32_t rng_ = 1;
    State state_ = State::Playing;
    uint8_t tiles_[W * H] = {};
    uint8_t flags_[W * H] = {};  // bit 0 visible, bit 1 explored
    std::vector<Monster> monsters_;
    std::vector<Item> items_;
    std::vector<InvItem> inv_;
    Player p_;
    std::vector<std::string> messages_;
    std::string killer_;
};

}  // namespace rogue
