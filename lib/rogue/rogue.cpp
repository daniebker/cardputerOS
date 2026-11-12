#include "rogue.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace rogue {

namespace {
const MonsterInfo kMonsters[] = {
    // glyph name        hp atk def  xp depth erratic
    {'r', "rat", 4, 2, 0, 2, 1, false},
    {'b', "bat", 5, 3, 0, 3, 1, true},
    {'g', "goblin", 9, 4, 1, 5, 2, false},
    {'s', "snake", 11, 6, 1, 7, 3, false},
    {'o', "orc", 15, 7, 2, 10, 3, false},
    {'k', "skeleton", 18, 8, 3, 14, 4, false},
    {'O', "ogre", 29, 11, 3, 22, 5, false},
    {'W', "wraith", 25, 12, 4, 28, 6, true},
    {'T', "troll", 40, 14, 5, 40, 7, false},
    {'D', "ember drake", 68, 17, 6, 100, 99, false},  // only placed as the Crown's guardian
};
constexpr int kDrake = 9;
constexpr int kFovRadius = 8;

struct Room {
    int x, y, w, h;
    int cx() const { return x + w / 2; }
    int cy() const { return y + h / 2; }
};

int sign(int v) { return (v > 0) - (v < 0); }
}  // namespace

const MonsterInfo& monsterInfo(int kind) { return kMonsters[kind]; }
int monsterKinds() { return (int)(sizeof kMonsters / sizeof kMonsters[0]); }

char itemGlyph(int kind) {
    switch (kind) {
        case Gold: return '$';
        case PotionHeal: case PotionMight: return '!';
        case ScrollTeleport: case ScrollMap: return '?';
        case Weapon: return ')';
        case Armor: return '[';
        default: return '*';
    }
}

std::string itemName(int kind, int amount) {
    static const char* const weapons[] = {"dagger", "short sword", "mace", "long sword", "war axe", "rune blade"};
    static const char* const armors[] = {"leather armor", "studded armor", "ring mail", "chain mail", "plate mail", "dwarven plate"};
    const int n = std::max(1, std::min(6, amount)) - 1;
    switch (kind) {
        case Gold: return std::to_string(amount) + " gold";
        case PotionHeal: return "healing potion";
        case PotionMight: return "potion of might";
        case ScrollTeleport: return "scroll of teleport";
        case ScrollMap: return "scroll of mapping";
        case Weapon: return std::string(weapons[n]) + " +" + std::to_string(amount);
        case Armor: return std::string(armors[n]) + " +" + std::to_string(amount);
        default: return "Ember Crown";
    }
}

uint32_t Game::rnd() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

int Game::rnd(int lo, int hi) { return hi <= lo ? lo : lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

const Monster* Game::monsterAt(int x, int y) const {
    for (auto& m : monsters_)
        if (m.x == x && m.y == y) return &m;
    return nullptr;
}

const Item* Game::itemAt(int x, int y) const {
    for (auto& it : items_)
        if (it.x == x && it.y == y) return &it;
    return nullptr;
}

std::vector<std::string> Game::takeMessages() {
    std::vector<std::string> out;
    out.swap(messages_);
    return out;
}

bool Game::freeCell(int x, int y) const {
    return walkable(x, y) && !monsterAt(x, y) && !(p_.x == x && p_.y == y);
}

bool Game::randomFloor(int& x, int& y, int avoidX, int avoidY, int minDist) {
    for (int tries = 0; tries < 400; tries++) {
        const int tx = rnd(1, W - 2), ty = rnd(1, H - 2);
        if (tile(tx, ty) != Floor || monsterAt(tx, ty) || itemAt(tx, ty)) continue;
        if (std::max(abs(tx - avoidX), abs(ty - avoidY)) < minDist) continue;
        x = tx;
        y = ty;
        return true;
    }
    return false;
}

// A monster kind suited to the current depth (mostly from the last few depths).
int Game::pickMonsterKind() {
    int kind = 0;
    for (int tries = 0; tries < 20; tries++) {
        kind = rnd(0, kDrake - 1);
        const int md = kMonsters[kind].minDepth;
        if (md <= p_.depth && md >= p_.depth - 3) break;
        if (md <= p_.depth && tries > 12) break;
    }
    return kMonsters[kind].minDepth > p_.depth ? 0 : kind;
}

void Game::newGame(uint32_t seed) {
    rng_ = seed ? seed : 0xC0FFEEu;
    state_ = State::Playing;
    p_ = Player();
    p_.hp = p_.maxHp = 20;
    inv_.clear();
    inv_.push_back({PotionHeal, 0});
    messages_.clear();
    killer_.clear();
    makeLevel();
    say("Find the Ember Crown on depth 8.");
}

void Game::makeLevel() {
    memset(tiles_, Rock, sizeof tiles_);
    memset(flags_, 0, sizeof flags_);
    monsters_.clear();
    items_.clear();

    // rooms
    std::vector<Room> rooms;
    for (int tries = 0; tries < 60 && rooms.size() < 9; tries++) {
        Room r;
        r.w = rnd(4, 10);
        r.h = rnd(3, 5);
        r.x = rnd(1, W - r.w - 1);
        r.y = rnd(1, H - r.h - 1);
        bool ok = true;
        for (auto& o : rooms)
            if (r.x <= o.x + o.w + 1 && o.x <= r.x + r.w + 1 && r.y <= o.y + o.h + 1 && o.y <= r.y + r.h + 1) ok = false;
        if (!ok) continue;
        rooms.push_back(r);
    }
    // order rooms left to right so corridors stay short, then carve
    std::sort(rooms.begin(), rooms.end(), [](const Room& a, const Room& b) { return a.cx() < b.cx(); });
    for (auto& r : rooms)
        for (int y = r.y; y < r.y + r.h; y++)
            for (int x = r.x; x < r.x + r.w; x++) tiles_[y * W + x] = Floor;
    auto carveH = [&](int x0, int x1, int y) {
        for (int x = std::min(x0, x1); x <= std::max(x0, x1); x++) tiles_[y * W + x] = Floor;
    };
    auto carveV = [&](int y0, int y1, int x) {
        for (int y = std::min(y0, y1); y <= std::max(y0, y1); y++) tiles_[y * W + x] = Floor;
    };
    for (size_t i = 1; i < rooms.size(); i++) {
        const Room& a = rooms[i - 1];
        const Room& b = rooms[i];
        if (rnd(0, 1)) {
            carveH(a.cx(), b.cx(), a.cy());
            carveV(a.cy(), b.cy(), b.cx());
        } else {
            carveV(a.cy(), b.cy(), a.cx());
            carveH(a.cx(), b.cx(), b.cy());
        }
    }
    // an extra loop so levels are not a single chain
    if (rooms.size() > 3) {
        const Room& a = rooms[0];
        const Room& b = rooms[rooms.size() / 2 + 1 < rooms.size() ? rooms.size() / 2 + 1 : rooms.size() - 1];
        carveV(a.cy(), b.cy(), a.cx());
        carveH(a.cx(), b.cx(), b.cy());
    }
    // walls around everything walkable
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            if (tiles_[y * W + x] != Rock) continue;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (inside(x + dx, y + dy) && tiles_[(y + dy) * W + x + dx] == Floor) tiles_[y * W + x] = Wall;
        }

    // start in a random end of the chain, goal in the other
    const bool flip = rnd(0, 1);
    const Room& start = flip ? rooms.back() : rooms.front();
    const Room& goal = flip ? rooms.front() : rooms.back();
    p_.x = start.cx();
    p_.y = start.cy();
    if (p_.depth < kMaxDepth) {
        tiles_[goal.cy() * W + goal.cx()] = Stairs;
    } else {
        items_.push_back({Crown, (int8_t)goal.cx(), (int8_t)goal.cy(), 0});
        Monster d;
        d.kind = kDrake;
        d.x = (int8_t)(goal.cx() + (goal.w > 4 ? 1 : 0));
        d.y = (int8_t)(goal.cy() + (goal.w > 4 ? 0 : 1));
        d.hp = (int16_t)kMonsters[kDrake].hp;
        monsters_.push_back(d);
    }

    // monsters: kinds from the last few depths, never next to the start
    const int count = 3 + p_.depth + rnd(0, 2);
    for (int i = 0; i < count; i++) {
        int x, y;
        if (!randomFloor(x, y, p_.x, p_.y, 6)) break;
        const int kind = pickMonsterKind();
        Monster m;
        m.kind = (uint8_t)kind;
        m.x = (int8_t)x;
        m.y = (int8_t)y;
        m.hp = (int16_t)kMonsters[kind].hp;
        m.awake = rnd(0, 9) < 3;
        monsters_.push_back(m);
    }

    // items
    const int nItems = 4 + rnd(0, 3);
    for (int i = 0; i < nItems; i++) {
        int x, y;
        if (!randomFloor(x, y, p_.x, p_.y, 2)) break;
        const int roll = rnd(0, 99);
        Item it;
        it.x = (int8_t)x;
        it.y = (int8_t)y;
        if (roll < 30) it.kind = PotionHeal;
        else if (roll < 56) { it.kind = Gold; it.amount = (int16_t)rnd(5, 20 + 10 * p_.depth); }
        else if (roll < 64) it.kind = ScrollTeleport;
        else if (roll < 72) it.kind = ScrollMap;
        else if (roll < 84) { it.kind = Weapon; it.amount = (int16_t)std::min(6, 1 + rnd(0, p_.depth / 2)); }
        else if (roll < 96) { it.kind = Armor; it.amount = (int16_t)std::min(6, 1 + rnd(0, p_.depth / 2)); }
        else it.kind = PotionMight;
        items_.push_back(it);
    }
    updateFov();
}

void Game::updateFov() {
    for (int i = 0; i < W * H; i++) flags_[i] &= ~1;
    for (int ty = p_.y - kFovRadius; ty <= p_.y + kFovRadius; ty++) {
        for (int tx = p_.x - kFovRadius; tx <= p_.x + kFovRadius; tx++) {
            if (!inside(tx, ty)) continue;
            const int dx = tx - p_.x, dy = ty - p_.y;
            if (dx * dx + dy * dy > kFovRadius * kFovRadius + 2) continue;
            // walk a line from the player; the target is seen if nothing opaque is in between
            int x = p_.x, y = p_.y;
            const int adx = abs(dx), ady = abs(dy), sx = sign(dx), sy = sign(dy);
            int err = adx - ady;
            bool clear = true;
            while (x != tx || y != ty) {
                if ((x != p_.x || y != p_.y) && !walkable(x, y)) {
                    clear = false;
                    break;
                }
                const int e2 = 2 * err;
                if (e2 > -ady) { err -= ady; x += sx; }
                if (e2 < adx) { err += adx; y += sy; }
            }
            if (clear) flags_[ty * W + tx] |= 3;
        }
    }
}

void Game::gainXp(int xp) {
    p_.xp += xp;
    while (p_.xp >= xpForNextLevel()) {
        p_.level++;
        p_.maxHp += 6;
        p_.hp = std::min(p_.maxHp, p_.hp + 6);
        say("You reach level " + std::to_string(p_.level) + "!");
    }
}

void Game::attackMonster(Monster& m) {
    const MonsterInfo& info = kMonsters[m.kind];
    m.awake = true;
    if (rnd(0, 99) >= 88) {
        say(std::string("You miss the ") + info.name + ".");
        return;
    }
    const int atk = attack();
    const int dmg = std::max(1, rnd((atk + 1) / 2, atk) - rnd(0, info.def));
    m.hp -= (int16_t)dmg;
    if (m.hp > 0) {
        say(std::string("You hit the ") + info.name + ".");
        return;
    }
    say(std::string("You kill the ") + info.name + "!");
    const int kind = m.kind;
    monsters_.erase(monsters_.begin() + (&m - &monsters_[0]));
    gainXp(kMonsters[kind].xp);
}

void Game::monsterAttacks(const Monster& m) {
    const MonsterInfo& info = kMonsters[m.kind];
    if (rnd(0, 99) >= 76) {
        say(std::string("The ") + info.name + " misses.");
        return;
    }
    const int dmg = std::max(1, rnd((info.atk + 1) / 2, info.atk) - rnd(0, defense()));
    p_.hp -= dmg;
    say(std::string("The ") + info.name + " hits you.");
    if (p_.hp <= 0) {
        p_.hp = 0;
        state_ = State::Dead;
        killer_ = info.name;
        say("You die...");
    }
}

void Game::monstersAct() {
    // distance from the player to every reachable cell (monsters walk downhill)
    static int16_t dist[W * H];
    for (int i = 0; i < W * H; i++) dist[i] = INT16_MAX;
    static int16_t queue[W * H];
    int head = 0, tail = 0;
    dist[p_.y * W + p_.x] = 0;
    queue[tail++] = (int16_t)(p_.y * W + p_.x);
    while (head < tail) {
        const int c = queue[head++];
        const int cx = c % W, cy = c / W;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                const int nx = cx + dx, ny = cy + dy;
                if ((dx || dy) && walkable(nx, ny) && dist[ny * W + nx] == INT16_MAX) {
                    dist[ny * W + nx] = (int16_t)(dist[c] + 1);
                    queue[tail++] = (int16_t)(ny * W + nx);
                }
            }
    }

    for (size_t i = 0; i < monsters_.size() && state_ == State::Playing; i++) {
        Monster& m = monsters_[i];
        if (!m.awake) {
            // notices the player when in view; light sleepers notice sooner
            if (visible(m.x, m.y) && rnd(0, 99) < 45) m.awake = true;
            continue;
        }
        if (std::max(abs(m.x - p_.x), abs(m.y - p_.y)) == 1) {
            monsterAttacks(m);
            continue;
        }
        int bx = m.x, by = m.y;
        if (kMonsters[m.kind].erratic && rnd(0, 1)) {
            bx += rnd(-1, 1);
            by += rnd(-1, 1);
        } else {
            int best = dist[m.y * W + m.x];
            if (best > 30) continue;  // too far to care
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    const int nx = m.x + dx, ny = m.y + dy;
                    if ((dx || dy) && freeCell(nx, ny) && dist[ny * W + nx] < best) {
                        best = dist[ny * W + nx];
                        bx = nx;
                        by = ny;
                    }
                }
        }
        if (freeCell(bx, by)) {
            m.x = (int8_t)bx;
            m.y = (int8_t)by;
        }
    }
}

void Game::endTurn() {
    p_.turns++;
    if (state_ != State::Playing) return;
    monstersAct();
    if (state_ != State::Playing) return;
    const int every = std::max(4, 13 - p_.level);
    if (p_.turns % every == 0 && p_.hp < p_.maxHp) p_.hp++;
    // The dungeon does not sleep: now and then something comes looking for you,
    // so resting forever is not safe.
    if (rnd(0, 159) == 0 && monsters_.size() < 30) {
        int x, y;
        if (randomFloor(x, y, p_.x, p_.y, 10)) {
            Monster m;
            m.kind = (uint8_t)pickMonsterKind();
            m.x = (int8_t)x;
            m.y = (int8_t)y;
            m.hp = (int16_t)kMonsters[m.kind].hp;
            m.awake = true;
            monsters_.push_back(m);
        }
    }
    updateFov();
}

bool Game::move(int dx, int dy) {
    if (state_ != State::Playing || (!dx && !dy)) return false;
    const int nx = p_.x + dx, ny = p_.y + dy;
    for (auto& m : monsters_) {
        if (m.x == nx && m.y == ny) {
            attackMonster(m);
            endTurn();
            return true;
        }
    }
    if (!walkable(nx, ny)) return false;
    p_.x = nx;
    p_.y = ny;
    if (const Item* it = itemAt(nx, ny)) {
        if (it->kind == Gold) {
            p_.gold += it->amount;
            say("You pick up " + itemName(Gold, it->amount) + ".");
            items_.erase(items_.begin() + (it - &items_[0]));
        } else {
            say("You see a " + itemName(it->kind, it->amount) + " here.");
        }
    } else if (tile(nx, ny) == Stairs) {
        say("There are stairs down here.");
    }
    endTurn();
    return true;
}

bool Game::rest() {
    if (state_ != State::Playing) return false;
    endTurn();
    return true;
}

bool Game::pickUp() {
    if (state_ != State::Playing) return false;
    const Item* it = itemAt(p_.x, p_.y);
    if (!it) {
        say("There is nothing here to pick up.");
        return false;
    }
    if (it->kind == Crown) {
        say("You take the Ember Crown. You win!");
        state_ = State::Won;
        items_.erase(items_.begin() + (it - &items_[0]));
        return true;
    }
    if ((int)inv_.size() >= kMaxInventory) {
        say("Your pack is full.");
        return false;
    }
    inv_.push_back({it->kind, it->amount});
    say("You pick up the " + itemName(it->kind, it->amount) + ".");
    items_.erase(items_.begin() + (it - &items_[0]));
    endTurn();
    return true;
}

bool Game::descend() {
    if (state_ != State::Playing) return false;
    if (tile(p_.x, p_.y) != Stairs) {
        say("There are no stairs here.");
        return false;
    }
    p_.depth++;
    makeLevel();
    say("You descend to depth " + std::to_string(p_.depth) + ".");
    p_.turns++;
    return true;
}

bool Game::use(int index) {
    if (state_ != State::Playing || index < 0 || index >= (int)inv_.size()) return false;
    const InvItem it = inv_[index];
    inv_.erase(inv_.begin() + index);
    switch (it.kind) {
        case PotionHeal: {
            const int heal = std::max(12, p_.maxHp / 2);
            p_.hp = std::min(p_.maxHp, p_.hp + heal);
            say("You feel better.");
            break;
        }
        case PotionMight:
            p_.might++;
            say("You feel stronger!");
            break;
        case ScrollTeleport: {
            int x, y;
            if (randomFloor(x, y, p_.x, p_.y, 8) || randomFloor(x, y, p_.x, p_.y, 1)) {
                p_.x = x;
                p_.y = y;
            }
            say("You are somewhere else.");
            break;
        }
        case ScrollMap:
            for (int i = 0; i < W * H; i++)
                if (tiles_[i] != Rock) flags_[i] |= 2;
            say("The level's layout fills your mind.");
            break;
        case Weapon:
            if (p_.weapon) inv_.push_back({Weapon, (int16_t)p_.weapon});
            p_.weapon = it.amount;
            say("You wield the " + itemName(Weapon, it.amount) + ".");
            break;
        case Armor:
            if (p_.armor) inv_.push_back({Armor, (int16_t)p_.armor});
            p_.armor = it.amount;
            say("You put on the " + itemName(Armor, it.amount) + ".");
            break;
        default:
            inv_.insert(inv_.begin() + index, it);
            return false;
    }
    endTurn();
    return true;
}

bool Game::drop(int index) {
    if (state_ != State::Playing || index < 0 || index >= (int)inv_.size()) return false;
    if (itemAt(p_.x, p_.y) || tile(p_.x, p_.y) == Stairs) {
        say("There is no room to drop that here.");
        return false;
    }
    const InvItem it = inv_[index];
    inv_.erase(inv_.begin() + index);
    items_.push_back({it.kind, (int8_t)p_.x, (int8_t)p_.y, it.amount});
    say("You drop the " + itemName(it.kind, it.amount) + ".");
    endTurn();
    return true;
}

// ------------------------------------------------------------ save / load

namespace {
constexpr uint8_t kSaveVersion = 1;
void put16(std::string& s, int v) {
    s.push_back((char)(v & 0xFF));
    s.push_back((char)((v >> 8) & 0xFF));
}
void put32(std::string& s, uint32_t v) {
    put16(s, (int)(v & 0xFFFF));
    put16(s, (int)(v >> 16));
}
struct Reader {
    const std::string& s;
    size_t pos = 0;
    bool ok = true;
    explicit Reader(const std::string& str) : s(str) {}
    int u8() {
        if (pos >= s.size()) {
            ok = false;
            return 0;
        }
        return (uint8_t)s[pos++];
    }
    int i16() {
        const int lo = u8(), hi = u8();
        return (int16_t)(lo | (hi << 8));
    }
    uint32_t u32() {
        const uint32_t lo = (uint16_t)i16(), hi = (uint16_t)i16();
        return lo | (hi << 16);
    }
};
}  // namespace

std::string Game::save() const {
    std::string s;
    s.push_back((char)kSaveVersion);
    put32(s, rng_);
    s.push_back((char)state_);
    for (int v : {p_.x, p_.y, p_.hp, p_.maxHp, p_.level, p_.xp, p_.gold, p_.weapon, p_.armor, p_.might, p_.depth}) put16(s, v);
    put32(s, (uint32_t)p_.turns);
    s.append((const char*)tiles_, sizeof tiles_);
    s.append((const char*)flags_, sizeof flags_);
    put16(s, (int)monsters_.size());
    for (auto& m : monsters_) {
        s.push_back((char)m.kind);
        s.push_back((char)m.x);
        s.push_back((char)m.y);
        put16(s, m.hp);
        s.push_back((char)m.awake);
    }
    put16(s, (int)items_.size());
    for (auto& it : items_) {
        s.push_back((char)it.kind);
        s.push_back((char)it.x);
        s.push_back((char)it.y);
        put16(s, it.amount);
    }
    put16(s, (int)inv_.size());
    for (auto& it : inv_) {
        s.push_back((char)it.kind);
        put16(s, it.amount);
    }
    return s;
}

bool Game::load(const std::string& data) {
    Reader r(data);
    if (r.u8() != kSaveVersion) return false;
    Game g;
    g.rng_ = r.u32();
    const int st = r.u8();
    if (st > (int)State::Won) return false;
    g.state_ = (State)st;
    int* fields[] = {&g.p_.x, &g.p_.y, &g.p_.hp, &g.p_.maxHp, &g.p_.level, &g.p_.xp, &g.p_.gold, &g.p_.weapon, &g.p_.armor, &g.p_.might, &g.p_.depth};
    for (int* f : fields) *f = r.i16();
    g.p_.turns = (int)r.u32();
    if (!r.ok || data.size() < r.pos + sizeof tiles_ + sizeof flags_) return false;
    memcpy(g.tiles_, data.data() + r.pos, sizeof tiles_);
    r.pos += sizeof tiles_;
    memcpy(g.flags_, data.data() + r.pos, sizeof flags_);
    r.pos += sizeof flags_;
    for (int i = 0; i < W * H; i++)
        if (g.tiles_[i] > Stairs) return false;
    int n = r.i16();
    if (n < 0 || n > 200) return false;
    for (int i = 0; i < n; i++) {
        Monster m;
        m.kind = (uint8_t)r.u8();
        m.x = (int8_t)r.u8();
        m.y = (int8_t)r.u8();
        m.hp = (int16_t)r.i16();
        m.awake = r.u8() != 0;
        if (m.kind >= monsterKinds() || !inside(m.x, m.y)) return false;
        g.monsters_.push_back(m);
    }
    n = r.i16();
    if (n < 0 || n > 400) return false;
    for (int i = 0; i < n; i++) {
        Item it;
        it.kind = (uint8_t)r.u8();
        it.x = (int8_t)r.u8();
        it.y = (int8_t)r.u8();
        it.amount = (int16_t)r.i16();
        if (it.kind >= kItemKinds || !inside(it.x, it.y)) return false;
        g.items_.push_back(it);
    }
    n = r.i16();
    if (n < 0 || n > kMaxInventory + 2) return false;
    for (int i = 0; i < n; i++) {
        InvItem it;
        it.kind = (uint8_t)r.u8();
        it.amount = (int16_t)r.i16();
        if (it.kind >= kItemKinds) return false;
        g.inv_.push_back(it);
    }
    if (!r.ok || r.pos != data.size()) return false;
    if (!inside(g.p_.x, g.p_.y) || !g.walkable(g.p_.x, g.p_.y) || g.p_.maxHp <= 0 || g.p_.level < 1 || g.p_.depth < 1 ||
        g.p_.depth > kMaxDepth || g.p_.level > 60 || g.p_.hp <= 0 || g.p_.hp > g.p_.maxHp || g.p_.maxHp > 2000 ||
        g.p_.xp < 0 || g.p_.gold < 0 || g.p_.weapon < 0 || g.p_.weapon > 20 || g.p_.armor < 0 || g.p_.armor > 20 ||
        g.p_.might < 0 || g.p_.might > 100)
        return false;
    *this = g;
    return true;
}

}  // namespace rogue
