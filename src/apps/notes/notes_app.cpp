// Notes: Markdown notes, a vim-style editor, and sync with a WebDAV folder of .md files.
#include <ctime>
#include <string>
#include <vector>

#include "../../os/kernel.h"
#include "../../os/icons.h"
#include "../../os/net.h"
#include "../../os/storage.h"
#include "../../os/theme.h"
#include "../../os/ui.h"
#include "notes_store.h"
#include "notes_sync.h"
#include "vim.h"
#include "vim_layout.h"
#include "vterm.h"

namespace {

constexpr uint32_t kSelectBg = theme::editorSelect;
constexpr uint32_t kEditorBg = theme::editorBg;

// Markdown highlighting colours
constexpr uint32_t kMdHeading = theme::mdHeading;
constexpr uint32_t kMdCode = theme::mdCode;
constexpr uint32_t kMdQuote = theme::mdQuote;
constexpr uint32_t kMdMarker = theme::mdMarker;

bool isFence(const Vim::Line& l) {
    size_t i = 0;
    while (i < l.size() && i < 3 && l[i] == ' ') i++;
    return l.size() >= i + 3 && ((l[i] == '`' && l[i + 1] == '`' && l[i + 2] == '`') ||
                                 (l[i] == '~' && l[i + 1] == '~' && l[i + 2] == '~'));
}

// Per-character colours for one line of Markdown. `inCode` = inside a fenced block.
void mdColors(const Vim::Line& l, bool inCode, std::vector<uint32_t>& out) {
    out.assign(l.size(), theme::text);
    if (inCode || isFence(l)) {
        std::fill(out.begin(), out.end(), kMdCode);
        return;
    }
    size_t i = 0;
    while (i < l.size() && l[i] == ' ') i++;
    // heading
    size_t h = i;
    while (h < l.size() && l[h] == '#') h++;
    if (h > i && h - i <= 6 && (h == l.size() || l[h] == ' ')) {
        std::fill(out.begin(), out.end(), kMdHeading);
        return;
    }
    // block quote
    if (i < l.size() && l[i] == '>') {
        std::fill(out.begin(), out.end(), kMdQuote);
        return;
    }
    // list marker: "- ", "* ", "+ ", "12. ", optionally followed by "[ ] " / "[x] "
    size_t m = i;
    if (m + 1 < l.size() && (l[m] == '-' || l[m] == '*' || l[m] == '+') && l[m + 1] == ' ') {
        m += 2;
    } else {
        size_t d = m;
        while (d < l.size() && l[d] >= '0' && l[d] <= '9') d++;
        if (d > m && d + 1 < l.size() && (l[d] == '.' || l[d] == ')') && l[d + 1] == ' ') m = d + 2;
    }
    if (m > i) {
        if (m + 3 < l.size() && l[m] == '[' && l[m + 2] == ']' && l[m + 3] == ' ') m += 4;
        for (size_t k = i; k < m; k++) out[k] = kMdMarker;
    }
    // inline `code`
    bool code = false;
    for (size_t k = m; k < l.size(); k++) {
        if (l[k] == '`') {
            out[k] = kMdCode;
            code = !code;
        } else if (code) {
            out[k] = kMdCode;
        }
    }
}

class NotesApp : public os::App {
public:
    void onStart() override {
        store_.load();
        if (SyncAccount::hasLegacyAccount()) {
            // The CalDAV account of an earlier version cannot be reused: start over with WebDAV.
            store_.detachAll();
            store_.save();
            SyncAccount::clearLegacy();
            migrated_ = true;
        }
        account_ = SyncAccount::load();
        large_ = storage::getInt("notes_font", 0) != 0;
    }

    void onResume() override {
        if (screen_ == Screen::Editor) {
            drawEditor(true);
            return;
        }
        drawList();
        if (migrated_) {
            migrated_ = false;
            ui::alert("Notes", "Notes now sync as Markdown (.md) files in a WebDAV folder instead of CalDAV. "
                               "Your notes are kept here. Press a to set up the folder; they will be uploaded to it.");
            drawList();
        }
        if (!autoSynced_ && account_.configured() && net::connected()) {
            autoSynced_ = true;
            sync(false);
        }
    }

    void onPause() override {
        // never lose text because the app was sent to the background or closed
        if (screen_ == Screen::Editor && vim_.modified()) saveNote();
    }

    void onKey(const key::Event& ev) override {
        if (screen_ == Screen::List) listKey(ev);
        else editorKey(ev);
    }

    const char* statusText() override {
        if (screen_ != Screen::Editor) return nullptr;
        status_ = NotesStore::titleOf(vim_.text());
        if (status_.empty()) status_ = "(new note)";
        if (status_.size() > 24) status_ = status_.substr(0, 24) + "..";
        return status_.c_str();
    }

private:
    enum class Screen { List, Editor };

    // ------------------------------------------------------------ list
    void drawList() {
        os::Gfx& g = os::gfx();
        os::Rect c = os::content();
        os::Rect r = ui::footer(g, c, "Enter: open  n: new  d: delete  s: sync  a: account");
        std::string title = "Notes";
        const int unsynced = store_.unsyncedCount();
        if (!account_.configured()) title += "   (no sync folder: press a)";
        else if (unsynced) title += "   " + std::to_string(unsynced) + " not synced";
        else if (!syncInfo_.empty()) title += "   " + syncInfo_;
        listRect_ = ui::header(g, r, title.c_str());

        std::string keep = (list_.selected < (int)ids_.size()) ? ids_[list_.selected] : std::string();
        list_.items.clear();
        ids_.clear();
        list_.emptyText = "No notes yet. Press n to write one.";
        for (Note* n : store_.list()) {
            ui::ListItem it;
            it.label = n->title.find_first_not_of(' ') == std::string::npos ? "(untitled)" : n->title;
            if (n->mtime > 1700000000) {
                struct tm t;
                localtime_r(&n->mtime, &t);
                char buf[16];
                strftime(buf, sizeof buf, "%b %d", &t);
                it.detail = buf;
            }
            if (n->dirty) it.detail += " *";
            list_.items.push_back(it);
            ids_.push_back(n->id);
        }
        for (size_t i = 0; i < ids_.size(); i++)
            if (ids_[i] == keep) list_.selected = (int)i;
        list_.draw(g, listRect_);
    }

    void listKey(const key::Event& ev) {
        os::Gfx& g = os::gfx();
        const bool onNote = list_.selected < (int)ids_.size();
        if (ev.is('n')) {
            openEditor(std::string());
        } else if (ev.is('s')) {
            sync(true);
        } else if (ev.is('a')) {
            setupAccount();
        } else if (ev.is('d') && onNote) {
            Note* n = store_.find(ids_[list_.selected]);
            if (n && ui::confirm("Delete note", "Delete \"" + n->title + "\"?")) {
                store_.removeByUser(n->id);
                store_.save();
                drawList();
                syncIfPossible();
            } else {
                drawList();
            }
        } else if (list_.onKey(g, listRect_, ev) == ui::List::Activated && onNote) {
            openEditor(ids_[list_.selected]);
        }
    }

    // ------------------------------------------------------------ sync
    void setupAccount() {
        SyncAccount a = account_;
        if (ui::prompt("WebDAV folder address", a.url, false,
                       "e.g. https://HOST/remote.php/dav/files/USER/Notes/ (Nextcloud)") &&
            ui::prompt("User name", a.user) &&
            ui::prompt("Password", a.pass, true, "Stored unencrypted on this device.")) {
            dav::Url u;
            if (!a.url.empty() && !dav::parseUrl(a.url, u)) {
                ui::alert("Account", "The address must start with https:// or http://");
            } else {
                const bool moved = a.url != account_.url;
                account_ = a;
                account_.save();
                if (moved) {
                    // a different folder: upload every note there
                    store_.detachAll();
                    store_.save();
                }
                syncInfo_.clear();
                if (account_.configured() && !u.https)
                    ui::alert("Account", "Note: with http:// the password and notes travel unencrypted.");
                drawList();
                if (account_.configured()) sync(true);
                return;
            }
        }
        drawList();
    }

    void syncIfPossible() {
        if (account_.configured() && net::connected()) sync(false);
    }

    // interactive = the user asked for it, so explain why it cannot run
    void sync(bool interactive) {
        if (!account_.configured() || !net::connected()) {
            if (interactive) {
                ui::alert("Sync", !account_.configured() ? "No sync folder yet. Press a to set one up."
                                                         : "Wi-Fi is not connected.");
                drawList();
            }
            return;
        }
        auto onCert = [](const std::string& host, const std::string& fp, bool changed) {
            std::string shown;
            for (size_t i = 0; i < fp.size(); i += 4) shown += fp.substr(i, 4) + " ";
            if (changed) {
                return ui::confirm("CERTIFICATE CHANGED",
                                   "The certificate of " + host +
                                       " is different from last time. That is normal after a renewal, but could also "
                                       "mean someone is intercepting the connection.\nSHA-256: " +
                                       shown + "\nTrust the new certificate?");
            }
            return ui::confirm("New server", "First connection to " + host + ".\nCertificate SHA-256:\n" + shown +
                                                 "\nTrust this server?");
        };
        SyncResult r = syncNotes(store_, account_, onCert, [](const std::string& msg) { ui::busy(msg); });
        if (r.ok) {
            time_t now = time(nullptr);
            struct tm t;
            localtime_r(&now, &t);
            char buf[48];
            snprintf(buf, sizeof buf, "synced %02d:%02d", t.tm_hour, t.tm_min);
            syncInfo_ = buf;
            if (r.conflicts) {
                ui::alert("Sync", std::to_string(r.conflicts) +
                                      " note(s) were changed here and on the server. Your version was kept as a "
                                      "separate \"(conflict copy)\" note.");
                // the copies are new local notes: send them too
                SyncResult again = syncNotes(store_, account_, onCert, [](const std::string& msg) { ui::busy(msg); });
                if (!again.ok) ui::alert("Sync failed", again.error);
            }
        } else if (r.folderMissing) {
            syncInfo_ = "sync failed";
            if (ui::confirm("Sync", "The folder " + account_.url + " does not exist on the server. Create it?", true)) {
                std::string err;
                ui::busy("Creating the folder...");
                if (createFolder(account_, onCert, err)) {
                    if (screen_ == Screen::List) drawList();
                    sync(interactive);
                    return;
                }
                ui::alert("Sync failed", err);
            }
        } else {
            syncInfo_ = "sync failed";
            ui::alert("Sync failed", r.error);
        }
        if (screen_ == Screen::List) drawList();
    }

    // ------------------------------------------------------------ editor
    void openEditor(const std::string& id) {
        editId_ = id;
        Note* n = id.empty() ? nullptr : store_.find(id);
        vim_.setText(n ? store_.text(*n) : std::string());
        saved_ = false;
        layout_.reset();
        screen_ = Screen::Editor;
        if (!n) {
            // a new note starts as a Markdown heading, in insert mode, ready to type the title
            vim_.setText("# ");
            key::Event e;
            e.code = 'A';
            vim_.onKey(e);
            vim_.setModified(false);
        }
        os::refreshStatus();
        drawEditor(true);
    }

    bool saveNote() {
        const std::string text = vim_.text();
        Note* n = editId_.empty() ? nullptr : store_.find(editId_);
        bool ok = true;
        if (!n) {
            if (text.find_first_not_of("# \n\t") == std::string::npos) {
                vim_.setModified(false);
                return true;  // nothing worth keeping
            }
            n = store_.create(text);
            editId_ = n->id;
        } else if (store_.text(*n) != text) {
            ok = store_.setText(*n, text);
        }
        ok = store_.save() && ok;
        if (ok) {
            vim_.setModified(false);
            saved_ = true;
        } else {
            vim_.setMessage("ERROR: could not write (storage full?)");
        }
        return ok;
    }

    void closeEditor() {
        screen_ = Screen::List;
        os::refreshStatus();
        os::Rect c = os::content();
        os::gfx().fillRect(c.x, c.y, c.w, c.h, theme::bg);
        drawList();
        if (saved_) syncIfPossible();
    }

    void editorKey(const key::Event& ev) {
        if (ev.isOpt('f')) {
            large_ = !large_;
            storage::putInt("notes_font", large_ ? 1 : 0);
            drawEditor(true);
            return;
        }
        switch (vim_.onKey(ev)) {
            case Vim::Request::Write:
                if (saveNote()) vim_.setMessage("written");
                break;
            case Vim::Request::WriteQuit:
                if (saveNote()) {
                    closeEditor();
                    return;
                }
                break;
            case Vim::Request::Quit:
            case Vim::Request::ForceQuit:
                vim_.setModified(false);
                closeEditor();
                return;
            default:
                break;
        }
        os::refreshStatus();
        drawEditor(false);
    }

    void drawEditor(bool full) {
        os::Gfx& g = os::gfx();
        const os::Rect c = os::content();
        const int cw = large_ ? 8 : 6, ch = large_ ? 16 : 10, glyphDy = large_ ? 0 : 1;
        const int statusH = 12;
        constexpr int kMargin = 6;  // keep text away from the panel edges
        constexpr int kTopGap = 4;  // keep the first line away from the panel edge
        const int cols = (c.w - 2 * kMargin) / cw, rows = (c.h - statusH - kTopGap) / ch;
        const int y0 = c.y + kTopGap;
        const int x0 = c.x + (c.w - cols * cw) / 2;
        vim_.setPageRows(rows);

        if (full || cw != spriteCw_) {
            row_.deleteSprite();
            row_.setColorDepth(16);
            row_.createSprite(cols * cw, ch);
            spriteCw_ = cw;
            hashes_.assign(rows + 1, 0);
            g.fillRect(c.x, c.y, c.w, c.h, kEditorBg);
            full = true;
        }
        row_.setFont(large_ ? (const lgfx::IFont*)&fonts::AsciiFont8x16 : (const lgfx::IFont*)&fonts::Font0);
        row_.setTextDatum(textdatum_t::top_left);

        const auto& lines = vim_.lines();
        const int nlines = (int)lines.size();
        const auto& layout = layout_.update(vim_, cols, rows);

        Vim::Pos selA, selB;
        bool selLines = false;
        const bool hasSel = vim_.selection(selA, selB, selLines);
        const bool insert = vim_.mode() == Vim::Mode::Insert;
        const bool promptMode = vim_.mode() == Vim::Mode::Command || vim_.mode() == Vim::Mode::Search;

        // fenced code state at the first visible line
        bool inCode = false;
        const int firstLine = layout.empty() || layout[0].line < 0 ? nlines : layout[0].line;
        for (int l = 0; l < firstLine && l < nlines; l++)
            if (isFence(lines[l])) inCode = !inCode;
        std::vector<uint32_t> colors;
        int colorsLine = -1;

        g.startWrite();
        for (int y = 0; y < rows; y++) {
            const VimRow& lr = layout[y];
            const int line = lr.line;
            // work out what this screen row shows, and hash it to skip unchanged rows
            uint32_t h = 2166136261u;
            auto mix = [&h](uint32_t v) { h = (h ^ v) * 16777619u; };
            const bool past = line < 0;
            const Vim::Line* text = past ? nullptr : &lines[line];
            const int start = lr.start;
            if (!past && y > 0 && layout[y - 1].line != line && layout[y - 1].line >= 0 && isFence(lines[layout[y - 1].line]))
                inCode = !inCode;  // the previous line was a fence
            const int cursorX = promptMode ? -1 : lr.cursor;
            mix(past ? 1 : 2);
            mix(inCode ? 7 : 3);
            mix((uint32_t)cursorX + (insert ? 1000 : 0));
            if (!past) {
                mix((uint32_t)lr.len);
                for (int i = 0; i < lr.len; i++) mix((*text)[start + i]);
                if (hasSel && line >= selA.r && line <= selB.r) {
                    mix(0xABCD);
                    mix(selLines);
                    mix(line == selA.r ? selA.c : -1);
                    mix(line == selB.r ? selB.c : -1);
                }
            }
            if (full || hashes_[y] != h) {
                hashes_[y] = h;
                // pushSprite sends the row in the background (DMA): the previous row
                // must be fully sent before the buffer is drawn into again
                g.waitDMA();
                row_.fillSprite(kEditorBg);
                if (past) {
                    row_.setTextColor(theme::editorTilde);
                    row_.drawChar('~', 0, glyphDy);
                } else {
                    if (colorsLine != line) {
                        mdColors(*text, inCode, colors);
                        colorsLine = line;
                    }
                    for (int i = 0; i < cols; i++) {
                        const int col = start + i;
                        const bool inText = i < lr.len;
                        bool selected = false;
                        if (hasSel && line >= selA.r && line <= selB.r && (inText || col == 0)) {
                            selected = selLines || ((line > selA.r || col >= selA.c) && (line < selB.r || col <= selB.c));
                        }
                        const bool cursorCell = i == cursorX;
                        uint32_t fg = inText ? colors[col] : theme::text;
                        if (selected) row_.fillRect(i * cw, 0, cw, ch, kSelectBg);
                        if (cursorCell && !insert) {
                            row_.fillRect(i * cw, 0, cw, ch, theme::editorCursor);
                            fg = kEditorBg;
                        }
                        if (inText) {
                            uint32_t cp = (*text)[col];
                            uint8_t gl = (cp >= 0x20 && cp < 0x7F) ? (uint8_t)cp : (cp == '\t' ? ' ' : VTerm::glyphFor(cp));
                            if (gl == 0 || gl >= 0x7F) gl = '?';
                            if (gl != ' ') {
                                row_.setTextColor(fg);
                                row_.drawChar(gl, i * cw, glyphDy);
                            }
                        }
                        if (cursorCell && insert) row_.fillRect(i * cw, 0, 2, ch, theme::editorCursor);
                    }
                }
                row_.pushSprite(&g, x0, y0 + y * ch);
            }
        }

        // --- status line ---
        std::string left = vim_.prompt();
        uint32_t leftColor = theme::text;
        if (!left.empty()) left += "_";
        else if (!vim_.message().empty()) {
            left = vim_.message();
            leftColor = theme::warn;
        } else if (insert) left = "-- INSERT --";
        else if (vim_.mode() == Vim::Mode::Visual) left = "-- VISUAL --";
        else if (vim_.mode() == Vim::Mode::VisualLine) left = "-- VISUAL LINE --";
        if (leftColor == theme::text && left.compare(0, 2, "--") == 0) leftColor = theme::ok;
        char pos[32];
        snprintf(pos, sizeof pos, "%s%d,%d", vim_.modified() ? "[+] " : "", vim_.row() + 1, vim_.col() + 1);
        uint32_t sh = 2166136261u;
        for (char ch2 : left + "|" + pos) sh = (sh ^ (uint8_t)ch2) * 16777619u;
        if (full || hashes_[rows] != sh) {
            hashes_[rows] = sh;
            const int sy = c.y + c.h - statusH;
            g.fillRect(c.x, sy, c.w, statusH, theme::panel);
            g.setFont(theme::fontSmall());
            g.setTextDatum(textdatum_t::top_left);
            const int pw = g.textWidth(pos);
            g.setTextColor(theme::dim);
            g.drawString(pos, c.x + c.w - pw - 4, sy + 1);
            g.setTextColor(leftColor);
            ui::drawClipped(g, left, c.x + 4, sy + 1, c.w - pw - 16);
        }
        g.endWrite();
    }

    Screen screen_ = Screen::List;
    NotesStore store_;
    SyncAccount account_;
    ui::List list_;
    std::vector<std::string> ids_;
    os::Rect listRect_{0, 0, 0, 0};
    std::string syncInfo_, status_;
    bool autoSynced_ = false;
    bool migrated_ = false;

    Vim vim_;
    std::string editId_;
    bool saved_ = false;
    bool large_ = false;
    VimLayout layout_;
    M5Canvas row_;
    int spriteCw_ = 0;
    std::vector<uint32_t> hashes_;
};

OS_REGISTER_APP(NotesApp, "Notes", "Markdown notes, WebDAV sync", theme::dracula::foreground, icons::note, 20);

}  // namespace
