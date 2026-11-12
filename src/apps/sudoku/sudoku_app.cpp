// Sudoku: generated puzzles with a unique solution, pencil marks, undo, hints.
// The game in progress is saved and restored, including across reboots.
#include <esp_random.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../../os/kernel.h"
#include "../../os/storage.h"
#include "../../os/theme.h"
#include "../../os/ui.h"
#include "sudoku.h"

namespace {

using sudoku::Difficulty;
using sudoku::Grid;

// --- layout (320x240 screen) ---
constexpr int kCell = 24;                         // cell size in pixels
constexpr int kGap = 1;                           // line between cells
constexpr int kBoxGap = 3;                        // line between 3x3 boxes (and border)
constexpr int kGrid = 9 * kCell + 6 * kGap + 4 * kBoxGap;  // 234
constexpr int kGx = 3, kGy = 3;                   // grid origin
constexpr int kPanelX = kGx + kGrid + 6;          // side panel

inline int cellPos(int c) { return kBoxGap + c * (kCell + kGap) + (c / 3) * (kBoxGap - kGap); }

// --- colours (Dracula) ---
namespace col {
constexpr uint32_t boxLine = theme::dracula::comment;
constexpr uint32_t cellLine = theme::dracula::currentLine;
constexpr uint32_t cell = theme::dracula::background;
constexpr uint32_t peer = 0x343746;
constexpr uint32_t same = theme::dracula::currentLine;
constexpr uint32_t selected = 0x5B4E8C;
constexpr uint32_t wrongBg = 0x5C2E3A;
constexpr uint32_t given = theme::dracula::foreground;
constexpr uint32_t entered = theme::dracula::cyan;
constexpr uint32_t conflict = theme::dracula::red;
constexpr uint32_t note = theme::dracula::comment;
constexpr uint32_t noteHi = theme::dracula::yellow;
}  // namespace col

const char* const kDiffNames[] = {"Easy", "Medium", "Hard"};

std::string mmss(uint32_t s) {
    char buf[16];
    if (s >= 3600) snprintf(buf, sizeof buf, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    else snprintf(buf, sizeof buf, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    return buf;
}

class SudokuApp : public os::App {
public:
    void onStart() override { haveGame_ = load(); }

    ~SudokuApp() override { save(); }

    void onResume() override {
        if (!haveGame_) {
            drawBackground();
            if (!newGame(false)) {
                finish();
                return;
            }
        }
        lastTick_ = millis();
        drawAll();
    }

    void onPause() override { save(); }

    const char* statusText() override {
        if (!haveGame_) return nullptr;
        status_ = std::string(kDiffNames[(int)diff_]) + "  " + mmss(elapsed_ / 1000) + (solved_ ? "  solved" : "") +
                  (pencil_ ? "  pencil" : "");
        return status_.c_str();
    }

    void onTick() override {
        if (!haveGame_) return;
        const uint32_t now = millis();
        if (!solved_) elapsed_ += now - lastTick_;
        lastTick_ = now;
        const uint32_t sec = elapsed_ / 1000;
        if (sec != shownSec_) {
            shownSec_ = sec;
            drawTime();
        }
        if (dirty_ && now - lastSave_ > 30000) save();
    }

    void onKey(const key::Event& ev) override {
        if (!haveGame_) return;
        const uint16_t k = ev.code;
        int dr = 0, dc = 0;
        if (ev.is(key::Up) || ev.is('k') || ev.is('w')) dr = -1;
        else if (ev.is(key::Down) || ev.is('j') || ev.is('s')) dr = 1;
        else if (ev.is(key::Left) || ev.is('h') || ev.is('a')) dc = -1;
        else if (ev.is(key::Right) || ev.is('l') || ev.is('d')) dc = 1;
        if (dr || dc) {
            int r = (sel_ / 9 + dr + 9) % 9, c = (sel_ % 9 + dc + 9) % 9;  // wraps around
            sel_ = r * 9 + c;
            drawCells();
            return;
        }
        if (ev.is('N') || (solved_ && ev.is('n'))) {
            if (solved_ || ui::confirm("New game", "Give up this puzzle and start a new one?")) newGame(true);
            drawAll();
            return;
        }
        if (ev.is('?')) {
            hint();
            return;
        }
        if (solved_) return;
        static const char shifted[] = "!@#$%^&*(";  // shift + 1..9
        if (k >= '1' && k <= '9' && ev.isChar()) {
            if (pencil_) toggleNote(sel_, k - '0');
            else place(sel_, k - '0');
        } else if (ev.isChar() && k < 0x80 && strchr(shifted, (char)k) && k) {
            toggleNote(sel_, (int)(strchr(shifted, (char)k) - shifted) + 1);
        } else if (ev.is('0') || ev.code == key::Backspace || ev.code == key::Delete || ev.is('x') || ev.is(' ')) {
            place(sel_, 0);
        } else if (ev.is('p') || ev.is(key::Tab)) {
            pencil_ = !pencil_;
            drawPanel();
            os::refreshStatus();
        } else if (ev.is('u')) {
            undo();
        } else if (ev.is('c')) {
            check();
        } else if (ev.is('m')) {
            autoNotes();
        }
    }

private:
    struct Change {
        int cell;
        uint8_t value;
        uint16_t notes;
    };
    using Move = std::vector<Change>;  // everything one action changed, to undo it

    // ------------------------------------------------------------ game logic
    bool newGame(bool ask) {
        (void)ask;
        int choice = ui::choose("New Sudoku", {"Easy", "Medium", "Hard"}, haveGame_ ? (int)diff_ : 0);
        if (choice < 0) return haveGame_;
        diff_ = (Difficulty)choice;
        seed_ = esp_random();
        ui::busy("Creating a puzzle...");
        Grid solution;
        sudoku::generate(diff_, seed_, puzzle_, solution);
        solution_ = solution;
        values_ = puzzle_;
        notes_.fill(0);
        undo_.clear();
        elapsed_ = 0;
        shownSec_ = UINT32_MAX;
        hints_ = 0;
        solved_ = false;
        pencil_ = false;
        checked_ = false;
        sel_ = 40;
        haveGame_ = true;
        lastTick_ = millis();
        dirty_ = true;
        save();
        os::refreshStatus();
        return true;
    }

    void record(Move& m, int cell) {
        for (auto& ch : m)
            if (ch.cell == cell) return;
        m.push_back({cell, values_[cell], notes_[cell]});
    }

    void commit(Move&& m) {
        if (m.empty()) return;
        undo_.push_back(std::move(m));
        if (undo_.size() > 300) undo_.erase(undo_.begin());
        dirty_ = true;
        checked_ = false;
    }

    void place(int i, int d) {
        if (puzzle_[i]) return;  // givens are fixed
        if (values_[i] == d && notes_[i] == 0) return;
        Move m;
        record(m, i);
        values_[i] = (uint8_t)d;
        notes_[i] = 0;
        if (d) {
            // a placed digit removes that pencil mark from the cell's row, column and box
            for (int j = 0; j < 81; j++) {
                if (j != i && sudoku::sees(i, j) && (notes_[j] & (1u << d))) {
                    record(m, j);
                    notes_[j] &= ~(1u << d);
                }
            }
        }
        commit(std::move(m));
        afterChange();
    }

    void toggleNote(int i, int d) {
        if (values_[i]) return;
        Move m;
        record(m, i);
        notes_[i] ^= (1u << d);
        commit(std::move(m));
        afterChange();
    }

    // Fills every empty cell's pencil marks with the digits the rules still allow.
    void autoNotes() {
        Move m;
        for (int i = 0; i < 81; i++) {
            if (values_[i]) continue;
            const uint16_t c = sudoku::candidates(values_, i);
            if (c != notes_[i]) {
                record(m, i);
                notes_[i] = c;
            }
        }
        commit(std::move(m));
        afterChange();
    }

    void undo() {
        if (undo_.empty()) return;
        for (auto& ch : undo_.back()) {
            values_[ch.cell] = ch.value;
            notes_[ch.cell] = ch.notes;
        }
        undo_.pop_back();
        dirty_ = true;
        checked_ = false;
        drawCells();
        drawPanel();
    }

    // Reveals the selected cell, or if it is already right, the first wrong or empty cell.
    void hint() {
        if (solved_) return;
        int target = -1;
        if (!puzzle_[sel_] && values_[sel_] != solution_[sel_]) target = sel_;
        for (int i = 0; i < 81 && target < 0; i++)
            if (values_[i] != solution_[i]) target = i;
        if (target < 0) return;
        sel_ = target;
        hints_++;
        place(target, solution_[target]);
    }

    void check() {
        checked_ = true;
        int wrong = 0;
        for (int i = 0; i < 81; i++)
            if (values_[i] && values_[i] != solution_[i]) wrong++;
        drawCells();
        drawPanel();
        if (wrong == 0) ui::alert("Check", "Everything so far is correct.");
        else ui::alert("Check", std::to_string(wrong) + (wrong == 1 ? " digit is" : " digits are") + " wrong (marked in red).");
        drawAll();
    }

    void afterChange() {
        bool full = true;
        for (int i = 0; i < 81; i++)
            if (values_[i] != solution_[i]) full = false;
        drawCells();
        drawPanel();
        if (full && !solved_) win();
    }

    void win() {
        solved_ = true;
        dirty_ = true;
        save();
        const uint32_t sec = elapsed_ / 1000;
        std::string key = "sdk_best" + std::to_string((int)diff_);
        const int best = storage::getInt(key.c_str(), 0);
        std::string msg = std::string(kDiffNames[(int)diff_]) + " puzzle solved in " + mmss(sec);
        if (hints_) msg += " with " + std::to_string(hints_) + (hints_ == 1 ? " hint" : " hints");
        msg += ".";
        if (hints_ == 0 && (best == 0 || (int)sec < best)) {
            storage::putInt(key.c_str(), (int)sec);
            if (best) msg += "\nNew best time! (was " + mmss(best) + ")";
        } else if (best) {
            msg += "\nBest time: " + mmss(best);
        }
        os::refreshStatus();
        ui::alert("Solved!", msg + "\nPress n for a new puzzle.");
        drawAll();
    }

    // ------------------------------------------------------------ persistence (NVS)
    // "v1|diff|seed|elapsed_s|hints|solved|81 digits|81 x 3 hex note masks"
    void save() {
        if (!haveGame_ || !dirty_) return;
        std::string s = "v1|" + std::to_string((int)diff_) + "|" + std::to_string(seed_) + "|" +
                        std::to_string(elapsed_ / 1000) + "|" + std::to_string(hints_) + "|" + (solved_ ? "1" : "0") + "|";
        for (int i = 0; i < 81; i++) s.push_back((char)('0' + values_[i]));
        s.push_back('|');
        char hex[4];
        for (int i = 0; i < 81; i++) {
            snprintf(hex, sizeof hex, "%03x", notes_[i]);
            s += hex;
        }
        storage::putStr("sudoku", s);
        dirty_ = false;
        lastSave_ = millis();
    }

    bool load() {
        auto f = storage::splitFields(storage::getStr("sudoku"), '|');
        if (f.size() != 8 || f[0] != "v1" || f[6].size() != 81 || f[7].size() != 243) return false;
        const int d = atoi(f[1].c_str());
        if (d < 0 || d > 2) return false;
        diff_ = (Difficulty)d;
        seed_ = (uint32_t)strtoul(f[2].c_str(), nullptr, 10);
        Grid solution;
        sudoku::generate(diff_, seed_, puzzle_, solution);  // same seed, same puzzle
        solution_ = solution;
        elapsed_ = (uint32_t)strtoul(f[3].c_str(), nullptr, 10) * 1000;
        hints_ = atoi(f[4].c_str());
        solved_ = f[5] == "1";
        for (int i = 0; i < 81; i++) {
            const int v = f[6][i] - '0';
            values_[i] = puzzle_[i] ? puzzle_[i] : (uint8_t)((v >= 0 && v <= 9) ? v : 0);
            notes_[i] = (uint16_t)strtoul(f[7].substr(i * 3, 3).c_str(), nullptr, 16) & 0x3FE;
        }
        sel_ = 40;
        return true;
    }

    // ------------------------------------------------------------ drawing
    void drawBackground() {
        os::Gfx& g = os::gfx();
        g.fillScreen(theme::bg);
    }

    void drawAll() {
        os::Gfx& g = os::gfx();
        g.startWrite();
        g.fillScreen(theme::bg);
        // lines: the grid area in box-line colour, each box in cell-line colour, cells on top
        g.fillRect(kGx, kGy, kGrid, kGrid, col::boxLine);
        for (int b = 0; b < 9; b++) {
            const int x = kGx + cellPos((b % 3) * 3), y = kGy + cellPos((b / 3) * 3);
            g.fillRect(x, y, 3 * kCell + 2 * kGap, 3 * kCell + 2 * kGap, col::cellLine);
        }
        g.endWrite();
        shown_.fill(UINT64_MAX);
        drawCells();
        drawPanel();
    }

    uint64_t cellState(int i) const {
        const int v = values_[i];
        const int sv = values_[sel_];
        uint64_t st = (uint64_t)v | ((uint64_t)notes_[i] << 4) | ((uint64_t)(puzzle_[i] != 0) << 14);
        if (i == sel_) st |= 1ull << 15;
        else if (sudoku::sees(i, sel_)) st |= 1ull << 16;
        if (sv && v == sv && i != sel_) st |= 1ull << 17;
        if (v && sudoku::conflicts(values_, i)) st |= 1ull << 18;
        if (checked_ && v && v != solution_[i]) st |= 1ull << 19;
        if (sv && !v && (notes_[i] & (1u << sv))) st |= 1ull << 20;  // pencil mark of the selected digit
        if (solved_) st |= 1ull << 21;
        return st;
    }

    void drawCells() {
        os::Gfx& g = os::gfx();
        g.startWrite();
        for (int i = 0; i < 81; i++) {
            const uint64_t st = cellState(i);
            if (st == shown_[i]) continue;
            shown_[i] = st;
            drawCell(g, i, st);
        }
        g.endWrite();
    }

    void drawCell(os::Gfx& g, int i, uint64_t st) {
        const int x = kGx + cellPos(i % 9), y = kGy + cellPos(i / 9);
        const bool sel = st & (1ull << 15), peer = st & (1ull << 16), same = st & (1ull << 17);
        const bool conflict = st & (1ull << 18), wrong = st & (1ull << 19);
        uint32_t bg = col::cell;
        if (peer) bg = col::peer;
        if (same) bg = col::same;
        if (wrong) bg = col::wrongBg;
        if (sel) bg = col::selected;
        g.fillRect(x, y, kCell, kCell, bg);
        const int v = values_[i];
        if (v) {
            g.setFont(&fonts::FreeSansBold12pt7b);
            g.setTextDatum(textdatum_t::middle_center);
            g.setTextColor(conflict || wrong ? col::conflict : (puzzle_[i] ? col::given : col::entered));
            char s[2] = {(char)('0' + v), 0};
            g.drawString(s, x + kCell / 2, y + kCell / 2 + 1);
        } else if (notes_[i]) {
            g.setFont(&fonts::Font0);
            g.setTextDatum(textdatum_t::top_left);
            const int sv = values_[sel_];
            for (int d = 1; d <= 9; d++) {
                if (!(notes_[i] & (1u << d))) continue;
                const int nx = x + 1 + ((d - 1) % 3) * 8, ny = y + ((d - 1) / 3) * 8;
                g.setTextColor(d == sv ? col::noteHi : col::note);
                g.drawChar('0' + d, nx + 1, ny);
            }
        }
    }

    void drawTime() {
        os::Gfx& g = os::gfx();
        const int w = g.width() - kPanelX;
        g.startWrite();
        g.fillRect(kPanelX, 24, w, 22, theme::bg);
        g.setFont(&fonts::DejaVu18);
        g.setTextDatum(textdatum_t::top_left);
        g.setTextColor(solved_ ? theme::ok : theme::text);
        g.drawString(mmss(elapsed_ / 1000).c_str(), kPanelX, 26);
        g.endWrite();
    }

    void drawPanel() {
        os::Gfx& g = os::gfx();
        const int w = g.width() - kPanelX;
        g.startWrite();
        g.fillRect(kPanelX, 0, w, g.height(), theme::bg);
        g.setTextDatum(textdatum_t::top_left);
        g.setFont(theme::fontUi());
        g.setTextColor(theme::accent);
        g.drawString(kDiffNames[(int)diff_], kPanelX, 6);
        g.endWrite();
        shownSec_ = UINT32_MAX;
        drawTime();
        g.startWrite();

        // mode
        g.setFont(theme::fontUi());
        g.setTextColor(pencil_ ? theme::warn : theme::dim);
        g.drawString(pencil_ ? "PENCIL" : "pen", kPanelX, 52);

        // digits still to place
        int count[10] = {0};
        for (int i = 0; i < 81; i++)
            if (values_[i] == solution_[i]) count[values_[i]]++;
        const int sv = values_[sel_];
        g.setFont(&fonts::FreeSansBold9pt7b);
        g.setTextDatum(textdatum_t::middle_center);
        for (int d = 1; d <= 9; d++) {
            const int cx = kPanelX + 12 + ((d - 1) % 3) * 24, cy = 84 + ((d - 1) / 3) * 20;
            const bool done = count[d] >= 9;
            if (d == sv) g.fillRoundRect(cx - 10, cy - 10, 20, 20, 3, theme::select);
            g.setTextColor(done ? theme::select : (d == sv ? theme::text : theme::dim));
            char s[2] = {(char)('0' + d), 0};
            g.drawString(s, cx, cy + 1);
        }

        // keys
        g.setFont(theme::fontSmall());
        g.setTextDatum(textdatum_t::top_left);
        g.setTextColor(theme::dim);
        // key / action pairs in two columns (the panel is only ~75 px wide)
        static const char* const keys[][2] = {{"1-9", "digit"}, {"0", "clear"}, {"p", "pencil"}, {"m", "marks"},
                                              {"u", "undo"},    {"?", "hint"},  {"c", "check"},  {"N", "new"}};
        int y = 140;
        for (auto& kv : keys) {
            g.setTextColor(theme::text);
            g.drawString(kv[0], kPanelX, y);
            g.setTextColor(theme::dim);
            g.drawString(kv[1], kPanelX + 26, y);
            y += 12;
        }
        g.endWrite();
    }

    Grid puzzle_{}, solution_{}, values_{};
    std::array<uint16_t, 81> notes_{};
    std::array<uint64_t, 81> shown_{};
    std::vector<Move> undo_;
    Difficulty diff_ = Difficulty::Easy;
    uint32_t seed_ = 0;
    uint32_t elapsed_ = 0, lastTick_ = 0, lastSave_ = 0, shownSec_ = UINT32_MAX;
    int hints_ = 0;
    int sel_ = 40;
    bool haveGame_ = false, solved_ = false, pencil_ = false, checked_ = false, dirty_ = false;
    std::string status_;
};

OS_REGISTER_APP(SudokuApp, "Sudoku", "Puzzle game", theme::dracula::pink);

}  // namespace
