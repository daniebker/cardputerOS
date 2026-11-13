// Ember Crown: a small roguelike. Descend eight levels and take the Crown.
// The run in progress is saved to flash; death deletes it (permadeath).
#include <esp_random.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../../os/kernel.h"
#include "../../os/icons.h"
#include "../../os/storage.h"
#include "../../os/theme.h"
#include "../../os/ui.h"
#include "rogue.h"

namespace {

using namespace rogue;
namespace dr = theme::dracula;

constexpr const char* kSavePath = "/rogue.sav";
constexpr int kCw = 8, kCh = 10;         // cell size: 40 x 24 cells on the 320 x 240 screen
constexpr int kCols = 40, kRows = 24;
constexpr int kMapRow = 2;               // rows 0-1: messages, 2-22: map, 23: stats
constexpr uint32_t kWallLit = 0x5A5E78, kWallDim = 0x34364A, kFloorLit = dr::comment, kFloorDim = 0x3A3D50;

struct Cell {
    char ch = ' ';
    uint32_t fg = 0, bg = 0;
    bool block = false;  // draw as a filled block (walls)
};

uint32_t monsterColor(int kind) {
    static const uint32_t c[] = {dr::orange, dr::orange, dr::green, dr::green, dr::red,
                                 dr::foreground, dr::red, dr::purple, dr::green, dr::red};
    return c[kind];
}

uint32_t itemColor(int kind) {
    switch (kind) {
        case Gold: return dr::yellow;
        case PotionHeal: case PotionMight: return dr::pink;
        case ScrollTeleport: case ScrollMap: return dr::cyan;
        case Crown: return dr::orange;
        default: return dr::green;
    }
}

class RogueApp : public os::App {
public:
    void onStart() override {
        std::string data;
        if (!storage::readFile(kSavePath, data) || !game_.load(data) || game_.state() != State::Playing) newGame();
        else pushMessage("Welcome back. Press ? for help.");
    }

    ~RogueApp() override { save(); }
    void onPause() override { save(); }

    void onResume() override {
        hashes_.assign(kRows, 0);
        os::gfx().fillScreen(theme::bg);
        draw(true);
    }

    const char* statusText() override {
        const Player& p = game_.player();
        status_ = "Depth " + std::to_string(p.depth) + "  HP " + std::to_string(p.hp) + "/" + std::to_string(p.maxHp) +
                  "  Lv " + std::to_string(p.level);
        return status_.c_str();
    }

    void onKey(const key::Event& ev) override {
        if (ev.ctrl() || ev.alt()) return;
        const uint16_t k = ev.code;
        int dx = 0, dy = 0;
        bool acted = false, handled = true;
        // movement: vi keys (with diagonals), arrows, and the arrow-legend keys without fn
        if (k == 'h' || k == key::Left || k == ',') dx = -1;
        else if (k == 'l' || k == key::Right || k == '/') dx = 1;
        else if (k == 'k' || k == key::Up || k == ';') dy = -1;
        else if (k == 'j' || k == key::Down || k == '.') dy = 1;
        else if (k == 'y') dx = -1, dy = -1;
        else if (k == 'u') dx = 1, dy = -1;
        else if (k == 'b') dx = -1, dy = 1;
        else if (k == 'n') dx = 1, dy = 1;
        else handled = false;

        if (handled) {
            acted = game_.move(dx, dy);
        } else if (k == 's' || k == ' ') {
            acted = game_.rest();
        } else if (k == 'g') {
            acted = game_.pickUp();
        } else if (k == '>' || k == key::Enter) {
            if (k == key::Enter && game_.itemAt(game_.player().x, game_.player().y)) acted = game_.pickUp();
            else acted = game_.descend();
            if (acted) hashes_.assign(kRows, 0);
        } else if (k == 'i' || k == 'd') {
            acted = inventory(k == 'd');
        } else if (k == '?') {
            help();
        } else if (k == 'N') {
            if (ui::confirm("New game", "Abandon this run and start a new one?")) newGame();
            onResume();
            return;
        } else {
            return;
        }
        (void)acted;
        collectMessages();
        draw(false);
        os::refreshStatus();
        if (game_.state() != State::Playing) gameOver();
        else if (game_.player().turns - lastSaveTurn_ >= 50) save();
    }

private:
    void newGame() {
        game_.newGame(esp_random());
        log_.clear();
        collectMessages();
        pushMessage("Press ? for help.");
        lastSaveTurn_ = 0;
        dirty_ = true;
        save();
        os::refreshStatus();
    }

    void save() {
        if (!dirty_ && game_.player().turns == lastSaveTurn_) return;
        if (game_.state() == State::Playing) storage::writeFile(kSavePath, game_.save());
        lastSaveTurn_ = game_.player().turns;
        dirty_ = false;
    }

    void pushMessage(const std::string& m) {
        log_.push_back(m);
        if (log_.size() > 6) log_.erase(log_.begin());
        fresh_++;
    }

    void collectMessages() {
        fresh_ = 0;
        for (auto& m : game_.takeMessages()) pushMessage(m);
        dirty_ = true;
    }

    void gameOver() {
        const Player& p = game_.player();
        storage::remove(kSavePath);  // permadeath: a finished run cannot be resumed
        const int deepest = std::max(storage::getInt("rogue_depth", 0), p.depth);
        storage::putInt("rogue_depth", deepest);
        std::string summary = "Level " + std::to_string(p.level) + ", " + std::to_string(p.gold) + " gold, " +
                              std::to_string(p.turns) + " turns.";
        if (game_.state() == State::Won) {
            const int wins = storage::getInt("rogue_wins", 0) + 1;
            storage::putInt("rogue_wins", wins);
            ui::alert("Victory!", "You carry the Ember Crown out of the depths.\n" + summary + "\nWins so far: " +
                                      std::to_string(wins));
        } else {
            ui::alert("You died", "Killed by a " + game_.causeOfDeath() + " on depth " + std::to_string(p.depth) + ".\n" +
                                      summary + "\nDeepest so far: " + std::to_string(deepest) +
                                      "   Wins: " + std::to_string(storage::getInt("rogue_wins", 0)));
        }
        newGame();
        onResume();
    }

    // Lets the player pick an item to use (or drop). Returns true if a turn passed.
    bool inventory(bool drop) {
        const auto& inv = game_.inventory();
        const Player& p = game_.player();
        if (inv.empty()) {
            pushMessage("Your pack is empty.");
            return false;
        }
        std::vector<std::string> names;
        for (auto& it : inv) names.push_back(itemName(it.kind, it.amount));
        std::string title = drop ? "Drop" : "Use";
        title += " (weapon +" + std::to_string(p.weapon) + ", armor +" + std::to_string(p.armor) + ")";
        const int pick = ui::choose(title.c_str(), names);
        bool acted = false;
        if (pick >= 0) acted = drop ? game_.drop(pick) : game_.use(pick);
        hashes_.assign(kRows, 0);
        os::gfx().fillScreen(theme::bg);
        return acted;
    }

    void help() {
        ui::alert("Ember Crown",
                  "Reach depth 8 and take the Crown (*).\n"
                  "Move: h j k l, diagonals y u b n, or ; . , / and the arrows. Walk into monsters to attack.\n"
                  "g get   i use/equip   d drop   s rest\n"
                  "> or Enter: stairs   N new game\n"
                  "! potion  ? scroll  ) weapon  [ armor  $ gold");
        hashes_.assign(kRows, 0);
        os::gfx().fillScreen(theme::bg);
    }

    // ------------------------------------------------------------ drawing
    void textRow(Cell* row, const std::string& s, uint32_t fg) {
        for (int i = 0; i < kCols && i < (int)s.size(); i++) {
            row[i].ch = s[i];
            row[i].fg = fg;
        }
    }

    void draw(bool full) {
        os::Gfx& g = os::gfx();
        if (!row_.getBuffer()) {
            row_.setColorDepth(16);
            row_.createSprite(kCols * kCw, kCh);
        }
        row_.setFont(&fonts::Font8x8C64);
        row_.setTextDatum(textdatum_t::top_left);
        const Player& p = game_.player();

        // messages: the newest that fit on two rows, word-wrapped; this turn's are brighter
        std::vector<std::string> lines;
        {
            std::string text;
            for (int i = (int)log_.size() - 1; i >= 0 && i >= (int)log_.size() - fresh_; i--) {
                std::string cand = log_[i] + (text.empty() ? "" : " " + text);
                if (cand.size() > 2 * kCols - 2) break;
                text = cand;
            }
            while (!text.empty() && lines.size() < 2) {
                if ((int)text.size() <= kCols) {
                    lines.push_back(text);
                    break;
                }
                size_t cut = text.rfind(' ', kCols);
                if (cut == std::string::npos) cut = kCols;
                lines.push_back(text.substr(0, cut));
                text = text.substr(cut + (text[cut] == ' ' ? 1 : 0));
            }
        }

        g.startWrite();
        for (int y = 0; y < kRows; y++) {
            Cell cells[kCols];
            for (auto& c : cells) c.bg = theme::bg;
            if (y < kMapRow) {
                if (y < (int)lines.size()) textRow(cells, lines[y], dr::foreground);
            } else if (y == kRows - 1) {
                char buf[64];
                snprintf(buf, sizeof buf, "HP %d/%d At%d Df%d Lv%d $%d", p.hp, p.maxHp, game_.attack(), game_.defense(), p.level, p.gold);
                textRow(cells, buf, p.hp * 100 < p.maxHp * 30 ? dr::red : dr::green);
                char dbuf[8];
                snprintf(dbuf, sizeof dbuf, "D%d", p.depth);
                const int len = (int)strlen(dbuf);
                for (int i = 0; i < len; i++) {
                    cells[kCols - len + i].ch = dbuf[i];
                    cells[kCols - len + i].fg = dr::purple;
                }
            } else {
                const int my = y - kMapRow;
                for (int x = 0; x < W; x++) {
                    Cell& c = cells[x];
                    if (!game_.explored(x, my)) continue;
                    const bool lit = game_.visible(x, my);
                    const Tile t = game_.tile(x, my);
                    if (t == Wall) {
                        c.block = true;
                        c.bg = lit ? kWallLit : kWallDim;
                    } else if (t == Floor) {
                        c.ch = '.';
                        c.fg = lit ? kFloorLit : kFloorDim;
                    } else if (t == Stairs) {
                        c.ch = '>';
                        c.fg = lit ? dr::yellow : dr::comment;
                    }
                    if (const Item* it = game_.itemAt(x, my)) {
                        c.ch = itemGlyph(it->kind);
                        c.fg = lit ? itemColor(it->kind) : dr::comment;
                    }
                    if (lit) {
                        if (const Monster* m = game_.monsterAt(x, my)) {
                            c.ch = monsterInfo(m->kind).glyph;
                            c.fg = monsterColor(m->kind);
                        }
                    }
                    if (x == p.x && my == p.y) {
                        c.ch = '@';
                        c.fg = theme::bg;
                        c.bg = dr::foreground;
                    }
                }
            }
            uint32_t h = 2166136261u;
            for (auto& c : cells) {
                h = (h ^ (uint8_t)c.ch) * 16777619u;
                h = (h ^ c.fg) * 16777619u;
                h = (h ^ c.bg) * 16777619u;
            }
            if (!full && hashes_[y] == h) continue;
            hashes_[y] = h;
            g.waitDMA();  // the previous row must be fully sent before the buffer is reused
            row_.fillSprite(theme::bg);
            for (int x = 0; x < kCols; x++) {
                const Cell& c = cells[x];
                if (c.bg != theme::bg) row_.fillRect(x * kCw, 0, kCw, kCh, c.bg);
                if (c.ch != ' ' && !c.block) {
                    row_.setTextColor(c.fg);
                    row_.drawChar(c.ch, x * kCw, 1);
                }
            }
            row_.pushSprite(&g, 0, y * kCh);
        }
        g.waitDMA();
        g.endWrite();
    }

    Game game_;
    std::vector<std::string> log_;
    int fresh_ = 0;
    int lastSaveTurn_ = 0;
    bool dirty_ = false;
    M5Canvas row_;
    std::vector<uint32_t> hashes_ = std::vector<uint32_t>(kRows, 0);
    std::string status_;
};

OS_REGISTER_APP_IN(RogueApp, "Games", "Ember Crown", "Roguelike dungeon crawl", theme::dracula::orange, icons::crown, 40);

}  // namespace
