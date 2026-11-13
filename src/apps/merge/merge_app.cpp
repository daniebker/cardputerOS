// Merge: sliding number tiles on a 4x4 board, aiming for a 2048 tile. The game in
// progress and the best score are kept across restarts.
#include <esp_random.h>

#include <string>

#include "../../os/icons.h"
#include "../../os/kernel.h"
#include "../../os/storage.h"
#include "../../os/theme.h"
#include "../../os/ui.h"
#include "merge.h"

namespace {

namespace dr = theme::dracula;
using merge::Dir;

// --- layout (320x240 screen) ---
constexpr int kTile = 53, kGap = 5;
constexpr int kBoard = merge::kSize * kTile + (merge::kSize + 1) * kGap;  // 237
constexpr int kBx = 1, kBy = 1;
constexpr int kPanelX = kBx + kBoard + 6;

// Tile colours by exponent: background, text.
struct TileColor {
    uint32_t bg, fg;
};
TileColor tileColor(uint8_t e) {
    static const TileColor table[] = {
        {dr::darker, dr::darker},       // empty
        {dr::currentLine, dr::foreground},  // 2
        {dr::comment, dr::foreground},      // 4
        {dr::cyan, dr::darkest},            // 8
        {dr::green, dr::darkest},           // 16
        {dr::yellow, dr::darkest},          // 32
        {dr::orange, dr::darkest},          // 64
        {dr::pink, dr::darkest},            // 128
        {dr::purple, dr::darkest},          // 256
        {dr::red, dr::foreground},          // 512
        {0xC678DD, dr::foreground},         // 1024
        {dr::foreground, dr::darkest},      // 2048
    };
    if (e < sizeof table / sizeof table[0]) return table[e];
    return {dr::darkest, dr::yellow};  // beyond 2048
}

class MergeApp : public os::App {
public:
    void onStart() override {
        game_ = merge::Game(esp_random());
        if (!game_.load(storage::getStr("merge"))) game_.reset();
        best_ = (uint32_t)storage::getInt("merge_best", 0);
        wonShown_ = game_.won();
    }

    ~MergeApp() override { save(); }

    void onResume() override { drawAll(); }
    void onPause() override { save(); }

    const char* statusText() override {
        status_ = "Score " + std::to_string(game_.score()) + "   best " + std::to_string(best_);
        return status_.c_str();
    }

    void onKey(const key::Event& ev) override {
        if (ev.ctrl() || ev.alt() || ev.opt()) return;
        const uint16_t k = ev.code;
        Dir d;
        if (k == key::Left || k == ',' || k == 'h' || k == 'a') d = Dir::Left;
        else if (k == key::Right || k == '/' || k == 'l' || k == 'd') d = Dir::Right;
        else if (k == key::Up || k == ';' || k == 'k' || k == 'w') d = Dir::Up;
        else if (k == key::Down || k == '.' || k == 'j' || k == 's') d = Dir::Down;
        else if (k == 'u' || k == key::Backspace) {
            if (game_.undo()) changed();
            return;
        } else if (k == 'n') {
            if (game_.over() || game_.score() == 0 || ui::confirm("New game", "Start a new game?")) {
                game_.reset();
                wonShown_ = false;
                changed();
            }
            drawAll();
            return;
        } else {
            return;
        }
        if (!game_.move(d)) return;
        changed();
        if (game_.won() && !wonShown_) {
            wonShown_ = true;
            ui::alert("2048!", "You made a 2048 tile with " + std::to_string(game_.score()) +
                                   " points.\nKeep going for a bigger one, or press n for a new game.");
            drawAll();
        }
    }

private:
    void changed() {
        if (game_.score() > best_) {
            best_ = game_.score();
            bestDirty_ = true;
        }
        if (++unsaved_ >= 25) save();
        drawBoard();
        drawPanel();
        os::refreshStatus();
    }

    void save() {
        storage::putStr("merge", game_.serialize());
        if (bestDirty_) storage::putInt("merge_best", (int)best_);
        bestDirty_ = false;
        unsaved_ = 0;
    }

    // ------------------------------------------------------------ drawing
    void drawAll() {
        os::Gfx& g = os::gfx();
        const os::Rect c = os::content();
        g.fillRect(c.x, c.y, c.w, c.h, theme::bg);
        drawBoard();
        drawPanel();
    }

    void drawTile(os::Gfx& g, int i) {
        const int x = kBx + kGap + (i % merge::kSize) * (kTile + kGap);
        const int y = kBy + kGap + (i / merge::kSize) * (kTile + kGap);
        const uint8_t e = game_.board()[i];
        const TileColor col = tileColor(e);
        g.fillRoundRect(x, y, kTile, kTile, 6, col.bg);
        if (i == game_.lastSpawn()) g.drawRoundRect(x, y, kTile, kTile, 6, dr::foreground);  // the new tile
        if (!e) return;
        const std::string text = merge::label(e);
        static const lgfx::IFont* const fonts[] = {&fonts::FreeSansBold18pt7b, &fonts::FreeSansBold12pt7b,
                                                   &fonts::FreeSansBold9pt7b, &fonts::DejaVu9};
        for (const lgfx::IFont* f : fonts) {
            g.setFont(f);
            if (g.textWidth(text.c_str()) <= kTile - 8) break;
        }
        g.setTextDatum(textdatum_t::middle_center);
        g.setTextColor(col.fg);
        g.drawString(text.c_str(), x + kTile / 2, y + kTile / 2 + 1);
    }

    void drawBoard() {
        os::Gfx& g = os::gfx();
        g.startWrite();
        g.fillRoundRect(kBx, kBy, kBoard, kBoard, 8, dr::darkest);
        for (int i = 0; i < merge::kCells; i++) drawTile(g, i);
        if (game_.over()) {
            const int w = kBoard - 40, h = 56, x = kBx + 20, y = kBy + (kBoard - h) / 2;
            g.fillRoundRect(x, y, w, h, 8, dr::darker);
            g.drawRoundRect(x, y, w, h, 8, dr::red);
            g.setTextDatum(textdatum_t::middle_center);
            g.setFont(&fonts::FreeSansBold12pt7b);
            g.setTextColor(dr::red);
            g.drawString("No moves left", x + w / 2, y + 18);
            g.setFont(theme::fontUi());
            g.setTextColor(theme::text);
            g.drawString("n: new game   u: undo", x + w / 2, y + 40);
        }
        g.endWrite();
    }

    void drawPanel() {
        os::Gfx& g = os::gfx();
        const os::Rect c = os::content();
        const int x = kPanelX, w = c.x + c.w - kPanelX - 2;
        g.startWrite();
        g.fillRect(x, c.y, w + 2, c.h, theme::bg);
        g.setTextDatum(textdatum_t::top_left);
        int y = c.y + 4;
        auto box = [&](const char* title, uint32_t value, uint32_t color) {
            g.fillRoundRect(x, y, w, 42, 6, dr::darker);
            g.setFont(theme::fontSmall());
            g.setTextColor(theme::dim);
            g.drawString(title, x + 6, y + 4);
            g.setFont(&fonts::FreeSansBold9pt7b);
            g.setTextColor(color);
            ui::drawClipped(g, std::to_string(value), x + 6, y + 19, w - 10);
            y += 48;
        };
        box("SCORE", game_.score(), theme::text);
        box("BEST", best_, dr::yellow);
        g.setFont(theme::fontSmall());
        static const char* const help[] = {"arrows: slide", "u: undo", "n: new game", "", "Reach 2048!"};
        for (const char* h : help) {
            g.setTextColor(h[0] == 'R' ? dr::purple : theme::dim);
            if (!(h[0] == 'u' && !game_.canUndo())) g.drawString(h, x + 2, y);
            y += 14;
        }
        g.endWrite();
    }

    merge::Game game_;
    uint32_t best_ = 0;
    bool bestDirty_ = false, wonShown_ = false;
    int unsaved_ = 0;
    std::string status_;
};

OS_REGISTER_APP_IN(MergeApp, "Games", "Merge", "Slide and merge number tiles", theme::dracula::cyan, icons::tiles, 35);

}  // namespace
