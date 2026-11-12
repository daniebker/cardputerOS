#include "vim.h"

#include <algorithm>
#include <cstdlib>

namespace {
constexpr size_t kMaxUndo = 60;
constexpr int kShiftWidth = 2;

bool isSpace(char16_t c) { return c == ' ' || c == '\t'; }
int cls(char16_t c, bool big) {
    if (isSpace(c)) return 0;
    if (big) return 1;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80) return 1;
    return 2;
}
bool lessPos(Vim::Pos a, Vim::Pos b) { return a.r < b.r || (a.r == b.r && a.c < b.c); }
char16_t lower(char16_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
}  // namespace

Vim::Vim() { lines_.push_back(Line()); }

// ------------------------------------------------------------ UTF-8 <-> UTF-16

Vim::Line Vim::fromUtf8(const std::string& s) {
    Line out;
    size_t i = 0;
    while (i < s.size()) {
        uint8_t c = (uint8_t)s[i];
        uint32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else { cp = 0xFFFD; n = 1; }
        bool bad = i + n > s.size();
        for (int k = 1; k < n && !bad; k++) {
            uint8_t cc = (uint8_t)s[i + k];
            if ((cc & 0xC0) != 0x80) bad = true;
            else cp = (cp << 6) | (cc & 0x3F);
        }
        if (bad) { cp = 0xFFFD; n = 1; }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back((char16_t)(0xD800 + (cp >> 10)));
            out.push_back((char16_t)(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back((char16_t)cp);
        }
    }
    return out;
}

std::string Vim::toUtf8(const Line& l) {
    std::string out;
    for (size_t i = 0; i < l.size(); i++) {
        uint32_t cp = l[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < l.size() && l[i + 1] >= 0xDC00 && l[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (l[i + 1] - 0xDC00);
            i++;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;  // lone surrogate
        }
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

void Vim::setText(const std::string& utf8) {
    lines_.clear();
    size_t pos = 0;
    for (;;) {
        size_t nl = utf8.find('\n', pos);
        std::string line = utf8.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines_.push_back(fromUtf8(line));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    // a trailing newline does not create an extra empty line
    if (lines_.size() > 1 && lines_.back().empty()) lines_.pop_back();
    cur_ = Pos();
    wantCol_ = 0;
    mode_ = Mode::Normal;
    modified_ = false;
    undo_.clear();
    redo_.clear();
    snap_ = false;
    resetPending();
    rec_.clear();
    message_.clear();
    revision_++;
}

std::string Vim::text() const {
    std::string out;
    for (size_t i = 0; i < lines_.size(); i++) {
        out += toUtf8(lines_[i]);
        if (i + 1 < lines_.size()) out.push_back('\n');
    }
    return out;
}

std::string Vim::prompt() const {
    if (mode_ == Mode::Command) return ":" + cmd_;
    if (mode_ == Mode::Search) return (searchForward_ ? "/" : "?") + cmd_;
    return std::string();
}

bool Vim::selection(Pos& a, Pos& b, bool& linewise) const {
    if (mode_ != Mode::Visual && mode_ != Mode::VisualLine) return false;
    a = anchor_;
    b = cur_;
    if (lessPos(b, a)) std::swap(a, b);
    linewise = mode_ == Mode::VisualLine;
    return true;
}

// ------------------------------------------------------------ primitives

int Vim::firstNonBlank(int r) const {
    const Line& l = lines_[r];
    int c = 0;
    while (c < (int)l.size() && isSpace(l[c])) c++;
    return c < (int)l.size() ? c : 0;
}

void Vim::clamp() {
    if (lines_.empty()) lines_.push_back(Line());
    cur_.r = std::max(0, std::min(cur_.r, lastRow()));
    int maxc = len(cur_.r);
    if (mode_ != Mode::Insert && maxc > 0) maxc--;
    cur_.c = std::max(0, std::min(cur_.c, maxc));
}

void Vim::touch() {
    if (!snap_) {
        snapLines_ = lines_;
        snapCur_ = cur_;
        snap_ = true;
    }
    modified_ = true;
    tick_++;
    revision_++;
}

Vim::Line Vim::cut(Pos a, Pos b, bool remove) {
    Line out;
    if (!lessPos(a, b)) return out;
    if (a.r == b.r) {
        out = lines_[a.r].substr(a.c, b.c - a.c);
        if (remove) {
            touch();
            lines_[a.r].erase(a.c, b.c - a.c);
        }
        return out;
    }
    out = lines_[a.r].substr(a.c);
    for (int r = a.r + 1; r < b.r; r++) {
        out.push_back('\n');
        out += lines_[r];
    }
    out.push_back('\n');
    out += lines_[b.r].substr(0, b.c);
    if (remove) {
        touch();
        lines_[a.r] = lines_[a.r].substr(0, a.c) + lines_[b.r].substr(b.c);
        lines_.erase(lines_.begin() + a.r + 1, lines_.begin() + b.r + 1);
    }
    return out;
}

void Vim::insertAt(Pos p, const Line& text, Pos* end) {
    touch();
    Line tail = lines_[p.r].substr(p.c);
    lines_[p.r].erase(p.c);
    size_t pos = 0;
    int r = p.r;
    for (;;) {
        size_t nl = text.find(u'\n', pos);
        if (nl == Line::npos) {
            lines_[r] += text.substr(pos);
            break;
        }
        lines_[r] += text.substr(pos, nl - pos);
        r++;
        lines_.insert(lines_.begin() + r, Line());
        pos = nl + 1;
    }
    if (end) *end = {r, len(r)};
    lines_[r] += tail;
}

Vim::Pos Vim::wordFwd(Pos p, bool big) const {
    if (p.c < len(p.r)) {
        int k = cls(lines_[p.r][p.c], big);
        while (k != 0 && p.c < len(p.r) && cls(lines_[p.r][p.c], big) == k) p.c++;
    }
    for (;;) {
        while (p.c < len(p.r) && isSpace(lines_[p.r][p.c])) p.c++;
        if (p.c < len(p.r)) return p;
        if (p.r == lastRow()) {
            p.c = len(p.r);
            return p;
        }
        p.r++;
        p.c = 0;
        if (len(p.r) == 0) return p;  // an empty line is a word
    }
}

Vim::Pos Vim::wordBack(Pos p, bool big) const {
    for (;;) {
        if (p.c == 0) {
            if (p.r == 0) return p;
            p.r--;
            p.c = len(p.r);
            if (p.c == 0) return p;
        }
        p.c--;
        while (p.c > 0 && isSpace(lines_[p.r][p.c])) p.c--;
        if (isSpace(lines_[p.r][p.c])) continue;  // only blanks before us on this line
        int k = cls(lines_[p.r][p.c], big);
        while (p.c > 0 && cls(lines_[p.r][p.c - 1], big) == k) p.c--;
        return p;
    }
}

Vim::Pos Vim::wordEnd(Pos p, bool big) const {
    p.c++;
    for (;;) {
        if (p.c >= len(p.r)) {
            if (p.r == lastRow()) {
                p.c = std::max(0, len(p.r) - 1);
                return p;
            }
            p.r++;
            p.c = 0;
            continue;
        }
        if (isSpace(lines_[p.r][p.c])) {
            p.c++;
            continue;
        }
        int k = cls(lines_[p.r][p.c], big);
        while (p.c + 1 < len(p.r) && cls(lines_[p.r][p.c + 1], big) == k) p.c++;
        return p;
    }
}

// ------------------------------------------------------------ undo

void Vim::commitUndo() {
    if (!snap_) return;
    snap_ = false;
    const std::vector<Line>& a = snapLines_;
    const std::vector<Line>& b = lines_;
    size_t pre = 0;
    while (pre < a.size() && pre < b.size() && a[pre] == b[pre]) pre++;
    size_t suf = 0;
    while (suf < a.size() - pre && suf < b.size() - pre && a[a.size() - 1 - suf] == b[b.size() - 1 - suf]) suf++;
    if (pre == a.size() && pre == b.size()) {
        snapLines_.clear();
        return;  // nothing actually changed
    }
    UndoEntry e;
    e.row = (int)pre;
    e.oldLines.assign(a.begin() + pre, a.end() - suf);
    e.newLines.assign(b.begin() + pre, b.end() - suf);
    e.before = snapCur_;
    e.after = cur_;
    undo_.push_back(std::move(e));
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
    std::vector<Line>().swap(snapLines_);
}

void Vim::undo() {
    if (undo_.empty()) {
        message_ = "Already at oldest change";
        return;
    }
    UndoEntry e = std::move(undo_.back());
    undo_.pop_back();
    lines_.erase(lines_.begin() + e.row, lines_.begin() + e.row + e.newLines.size());
    lines_.insert(lines_.begin() + e.row, e.oldLines.begin(), e.oldLines.end());
    cur_ = e.before;
    redo_.push_back(std::move(e));
    modified_ = true;
    revision_++;
    clamp();
}

void Vim::redo() {
    if (redo_.empty()) {
        message_ = "Already at newest change";
        return;
    }
    UndoEntry e = std::move(redo_.back());
    redo_.pop_back();
    lines_.erase(lines_.begin() + e.row, lines_.begin() + e.row + e.oldLines.size());
    lines_.insert(lines_.begin() + e.row, e.newLines.begin(), e.newLines.end());
    cur_ = e.after;
    undo_.push_back(std::move(e));
    modified_ = true;
    revision_++;
    clamp();
}

// ------------------------------------------------------------ key dispatch

void Vim::resetPending() {
    count_ = opCount_ = 0;
    hasCount_ = false;
    op_ = 0;
    prefix_ = 0;
}

bool Vim::idle() const { return mode_ == Mode::Normal && !op_ && !prefix_ && !hasCount_; }

Vim::Request Vim::onKey(const key::Event& ev) {
    if (ev.opt()) return Request::None;
    const bool wasIdle = idle();
    if (wasIdle) {
        rec_.clear();
        recTick_ = tick_;
    }
    if (!replaying_) rec_.push_back(ev);
    Request rq = handle(ev);
    if (mode_ != Mode::Insert) clamp();
    if (idle()) {
        commitUndo();
        if (!replaying_ && tick_ != recTick_) lastChange_ = rec_;
    }
    return rq;
}

Vim::Request Vim::handle(const key::Event& ev) {
    key::Event e = ev;
    if (e.isCtrl('[') || e.isCtrl('c')) {
        e.code = key::Esc;
        e.mods = 0;
    }
    switch (mode_) {
        case Mode::Insert:
            insertKey(e);
            return Request::None;
        case Mode::Command:
        case Mode::Search:
            return promptKey(e);
        default:
            return normalKey(e);
    }
}

void Vim::enterInsert() {
    mode_ = Mode::Insert;
    resetPending();
}

void Vim::leaveInsert() {
    mode_ = Mode::Normal;
    if (cur_.c > 0) cur_.c--;
    clamp();
    wantCol_ = cur_.c;
}

void Vim::insertKey(const key::Event& ev) {
    message_.clear();
    Line& l = lines_[cur_.r];
    if (ev.code == key::Esc) {
        leaveInsert();
    } else if (ev.code == key::Enter) {
        touch();
        // keep the indentation of the current line
        int ind = 0;
        while (ind < cur_.c && ind < (int)l.size() && isSpace(l[ind])) ind++;
        Line indent = l.substr(0, ind);
        Line tail = l.substr(cur_.c);
        l.erase(cur_.c);
        lines_.insert(lines_.begin() + cur_.r + 1, indent + tail);
        cur_.r++;
        cur_.c = ind;
    } else if (ev.code == key::Backspace || ev.isCtrl('h')) {
        if (cur_.c > 0) {
            touch();
            l.erase(cur_.c - 1, 1);
            cur_.c--;
        } else if (cur_.r > 0) {
            touch();
            int pl = len(cur_.r - 1);
            lines_[cur_.r - 1] += l;
            lines_.erase(lines_.begin() + cur_.r);
            cur_.r--;
            cur_.c = pl;
        }
    } else if (ev.code == key::Delete) {
        if (cur_.c < (int)l.size()) {
            touch();
            l.erase(cur_.c, 1);
        } else if (cur_.r < lastRow()) {
            touch();
            l += lines_[cur_.r + 1];
            lines_.erase(lines_.begin() + cur_.r + 1);
        }
    } else if (ev.isCtrl('w')) {
        if (cur_.c > 0) {
            Pos b = wordBack(cur_, false);
            if (b.r != cur_.r) b = {cur_.r, 0};
            cut(b, cur_, true);
            cur_ = b;
        }
    } else if (ev.isCtrl('u')) {
        if (cur_.c > 0) {
            cut({cur_.r, 0}, cur_, true);
            cur_.c = 0;
        }
    } else if (ev.code == key::Left) {
        if (cur_.c > 0) cur_.c--;
    } else if (ev.code == key::Right) {
        if (cur_.c < (int)l.size()) cur_.c++;
    } else if (ev.code == key::Up || ev.code == key::Down) {
        cur_.r = std::max(0, std::min(lastRow(), cur_.r + (ev.code == key::Up ? -1 : 1)));
        cur_.c = std::min(cur_.c, len(cur_.r));
    } else if (ev.code == key::Home) {
        cur_.c = 0;
    } else if (ev.code == key::End) {
        cur_.c = (int)l.size();
    } else if (ev.code == key::Tab) {
        touch();
        int n = 4 - (cur_.c % 4);
        l.insert(cur_.c, (size_t)n, u' ');
        cur_.c += n;
    } else if (ev.isChar()) {
        touch();
        l.insert(l.begin() + cur_.c, (char16_t)ev.code);
        cur_.c++;
    }
}

Vim::Request Vim::promptKey(const key::Event& ev) {
    if (ev.code == key::Esc) {
        mode_ = Mode::Normal;
        return Request::None;
    }
    if (ev.code == key::Backspace) {
        if (cmd_.empty()) mode_ = Mode::Normal;
        else cmd_.pop_back();
        return Request::None;
    }
    if (ev.code == key::Enter) {
        const bool isSearch = mode_ == Mode::Search;
        mode_ = Mode::Normal;
        if (isSearch) {
            if (!cmd_.empty()) {
                pattern_ = fromUtf8(cmd_);
                patternForward_ = searchForward_;
            }
            search(true, false);
            return Request::None;
        }
        return runCommand(cmd_);
    }
    if (ev.isChar() && cmd_.size() < 80) cmd_.push_back((char)ev.code);
    return Request::None;
}

Vim::Request Vim::runCommand(const std::string& raw) {
    std::string cmd = raw;
    while (!cmd.empty() && cmd.back() == ' ') cmd.pop_back();
    if (cmd.empty()) return Request::None;
    if (cmd == "w") return Request::Write;
    if (cmd == "wq" || cmd == "x" || cmd == "wq!" || cmd == "x!") return Request::WriteQuit;
    if (cmd == "q") {
        if (modified_) {
            message_ = "No write since last change (:q! discards, :wq saves)";
            return Request::None;
        }
        return Request::Quit;
    }
    if (cmd == "q!") return Request::ForceQuit;
    if (cmd.find_first_not_of("0123456789") == std::string::npos) {
        cur_.r = std::max(0, std::min(lastRow(), atoi(cmd.c_str()) - 1));
        cur_.c = firstNonBlank(cur_.r);
        return Request::None;
    }
    if (cmd == "$") {
        cur_.r = lastRow();
        cur_.c = firstNonBlank(cur_.r);
        return Request::None;
    }
    message_ = "Not an editor command: " + cmd;
    return Request::None;
}

// ------------------------------------------------------------ search / find

void Vim::search(bool sameDirection, bool) {
    if (pattern_.empty()) {
        message_ = "No previous search";
        return;
    }
    const bool forward = sameDirection ? patternForward_ : !patternForward_;
    bool ci = true;  // smartcase: case-insensitive unless the pattern has capitals
    for (char16_t c : pattern_)
        if (c >= 'A' && c <= 'Z') ci = false;
    auto matchAt = [&](int r, int c) {
        const Line& l = lines_[r];
        if (c < 0 || c + pattern_.size() > l.size()) return false;
        for (size_t i = 0; i < pattern_.size(); i++) {
            char16_t x = l[c + i], y = pattern_[i];
            if (ci ? lower(x) != lower(y) : x != y) return false;
        }
        return true;
    };
    const int n = (int)lines_.size();
    const Pos start = cur_;
    // Visit every line once starting at the cursor line, then the cursor line again
    // for the part on the other side of the cursor (wrap-around).
    for (int step = 0; step <= n; step++) {
        const int r = ((start.r + (forward ? step : -step)) % n + n) % n;
        const int L = len(r);
        int lo = 0, hi = L - 1;
        if (step == 0) (forward ? lo : hi) = forward ? start.c + 1 : start.c - 1;
        if (step == n) (forward ? hi : lo) = start.c;
        for (int c = forward ? lo : hi; forward ? c <= hi : c >= lo; c += forward ? 1 : -1) {
            if (!matchAt(r, c)) continue;
            const Pos found{r, c};
            if (forward && !lessPos(start, found)) message_ = "search hit BOTTOM, continuing at TOP";
            if (!forward && !lessPos(found, start)) message_ = "search hit TOP, continuing at BOTTOM";
            cur_ = found;
            wantCol_ = c;
            return;
        }
    }
    message_ = "Pattern not found: " + toUtf8(pattern_);
}

bool Vim::findChar(uint16_t kind, uint16_t ch, int count, Motion& m) {
    const Line& l = lines_[cur_.r];
    int c = cur_.c;
    const bool fwd = kind == 'f' || kind == 't';
    for (int i = 0; i < count; i++) {
        int k = c + (fwd ? 1 : -1);
        // t / T repeated with ; must skip the character right next to the cursor
        if ((kind == 't' || kind == 'T') && i == 0 && k >= 0 && k < (int)l.size() && l[k] == ch) k += fwd ? 1 : -1;
        while (k >= 0 && k < (int)l.size() && l[k] != ch) k += fwd ? 1 : -1;
        if (k < 0 || k >= (int)l.size()) return false;
        c = k;
    }
    if (kind == 't') c--;
    if (kind == 'T') c++;
    m.to = {cur_.r, c};
    m.ok = true;
    m.inclusive = fwd;
    return true;
}

// ------------------------------------------------------------ motions

bool Vim::motion(uint16_t k, int count, bool hasCount, bool forOp, Motion& m) {
    m = Motion();
    Pos p = cur_;
    const int L = len(p.r);
    switch (k) {
        case 'h': case key::Left: case key::Backspace:
            p.c = std::max(0, p.c - count);
            break;
        case 'l': case key::Right: case ' ':
            p.c = std::min(forOp ? L : std::max(0, L - 1), p.c + count);
            break;
        case 'j': case key::Down: case key::Enter:
            if (p.r == lastRow() && forOp) return false;
            p.r = std::min(lastRow(), p.r + count);
            p.c = (k == key::Enter) ? firstNonBlank(p.r) : wantCol_;
            m.linewise = true;
            break;
        case 'k': case key::Up:
            if (p.r == 0 && forOp) return false;
            p.r = std::max(0, p.r - count);
            p.c = wantCol_;
            m.linewise = true;
            break;
        case 'w': case 'W':
            for (int i = 0; i < count; i++) {
                Pos n = wordFwd(p, k == 'W');
                if (forOp && i == count - 1 && n.r != p.r) {
                    n = {p.r, len(p.r)};  // an operator stops at the end of the line
                }
                p = n;
            }
            break;
        case 'b': case 'B':
            for (int i = 0; i < count; i++) p = wordBack(p, k == 'B');
            break;
        case 'e': case 'E':
            for (int i = 0; i < count; i++) p = wordEnd(p, k == 'E');
            m.inclusive = true;
            break;
        case '0': case key::Home:
            p.c = 0;
            break;
        case '^':
            p.c = firstNonBlank(p.r);
            break;
        case '$': case key::End:
            p.r = std::min(lastRow(), p.r + count - 1);
            p.c = std::max(0, len(p.r) - 1);
            m.inclusive = true;
            if (len(p.r) == 0) m.inclusive = false;
            break;
        case 'G':
            p.r = hasCount ? std::min(lastRow(), count - 1) : lastRow();
            p.c = firstNonBlank(p.r);
            m.linewise = true;
            break;
        case 'g':  // gg
            p.r = hasCount ? std::min(lastRow(), count - 1) : 0;
            p.c = firstNonBlank(p.r);
            m.linewise = true;
            break;
        case '}':
            for (int i = 0; i < count; i++) {
                while (p.r < lastRow() && len(p.r) == 0) p.r++;
                while (p.r < lastRow() && len(p.r) != 0) p.r++;
            }
            p.c = (len(p.r) == 0) ? 0 : std::max(0, len(p.r) - (forOp ? 0 : 1));
            break;
        case '{':
            for (int i = 0; i < count; i++) {
                while (p.r > 0 && len(p.r) == 0) p.r--;
                while (p.r > 0 && len(p.r) != 0) p.r--;
            }
            p.c = 0;
            break;
        default:
            return false;
    }
    m.to = p;
    m.ok = true;
    return true;
}

bool Vim::textObject(uint16_t kind, bool around, Pos& a, Pos& b) {
    const Line& l = lines_[cur_.r];
    const int L = (int)l.size();
    if (L == 0) return false;
    int c = std::min(cur_.c, L - 1);
    if (kind == 'w' || kind == 'W') {
        const bool big = kind == 'W';
        int k = cls(l[c], big);
        int s = c, e = c + 1;
        while (s > 0 && cls(l[s - 1], big) == k) s--;
        while (e < L && cls(l[e], big) == k) e++;
        if (around) {
            if (k == 0) {
                // on blanks: include the following word
                if (e < L) {
                    int k2 = cls(l[e], big);
                    while (e < L && cls(l[e], big) == k2) e++;
                }
            } else if (e < L && isSpace(l[e])) {
                while (e < L && isSpace(l[e])) e++;
            } else {
                while (s > 0 && isSpace(l[s - 1])) s--;
            }
        }
        a = {cur_.r, s};
        b = {cur_.r, e};
        return true;
    }
    if (kind == '"' || kind == '\'' || kind == '`') {
        // find the pair of quotes around (or after) the cursor on this line
        int open = -1;
        for (int i = 0; i < L; i++) {
            if (l[i] != kind) continue;
            if (open < 0) {
                open = i;
            } else {
                if (c <= i) {
                    a = {cur_.r, around ? open : open + 1};
                    b = {cur_.r, around ? i + 1 : i};
                    return true;
                }
                open = -1;
            }
        }
        return false;
    }
    return false;
}

// ------------------------------------------------------------ operators

void Vim::shiftLines(int r0, int r1, bool right) {
    for (int r = r0; r <= r1; r++) {
        Line& l = lines_[r];
        if (right) {
            if (l.empty()) continue;
            touch();
            l.insert(0, (size_t)kShiftWidth, u' ');
        } else {
            int n = 0;
            while (n < kShiftWidth && n < (int)l.size() && l[n] == ' ') n++;
            if (n == 0 && !l.empty() && l[0] == '\t') n = 1;
            if (n) {
                touch();
                l.erase(0, n);
            }
        }
    }
    cur_ = {r0, firstNonBlank(r0)};
}

void Vim::applyLinewise(char op, int r0, int r1) {
    if (r0 > r1) std::swap(r0, r1);
    if (op == '>' || op == '<') {
        shiftLines(r0, r1, op == '>');
        return;
    }
    reg_.clear();
    for (int r = r0; r <= r1; r++) {
        reg_ += lines_[r];
        if (r < r1) reg_.push_back('\n');
    }
    regLinewise_ = true;
    if (op == 'y') {
        cur_.r = r0;
        if (r1 > r0) message_ = std::to_string(r1 - r0 + 1) + " lines yanked";
        return;
    }
    touch();
    if (op == 'd') {
        lines_.erase(lines_.begin() + r0, lines_.begin() + r1 + 1);
        if (lines_.empty()) lines_.push_back(Line());
        cur_.r = std::min(r0, lastRow());
        cur_.c = firstNonBlank(cur_.r);
    } else {  // c
        lines_.erase(lines_.begin() + r0 + 1, lines_.begin() + r1 + 1);
        lines_[r0].clear();
        cur_ = {r0, 0};
        enterInsert();
    }
}

void Vim::applyCharwise(char op, Pos a, Pos b) {
    if (op == '>' || op == '<') {
        shiftLines(a.r, b.r, op == '>');
        return;
    }
    if (!lessPos(a, b)) {
        if (op == 'c') enterInsert();
        return;
    }
    reg_ = cut(a, b, op != 'y');
    regLinewise_ = false;
    cur_ = a;
    if (op == 'c') enterInsert();
}

void Vim::applyOperator(char op, Pos from, const Motion& m) {
    if (m.linewise) {
        applyLinewise(op, from.r, m.to.r);
        return;
    }
    Pos a = from, b = m.to;
    if (lessPos(b, a)) std::swap(a, b);
    if (m.inclusive && b.c < len(b.r)) b.c++;
    else if (!m.inclusive && b.c == 0 && b.r > a.r) {
        // An exclusive motion ending at the start of a line stops at the end of the
        // previous one; if it also started in the indent it covers whole lines.
        if (a.c <= firstNonBlank(a.r)) {
            applyLinewise(op, a.r, b.r - 1);
            return;
        }
        b.r--;
        b.c = len(b.r);
    }
    applyCharwise(op, a, b);
}

void Vim::visualOperator(char op) {
    Pos a, b;
    bool linewise;
    if (!selection(a, b, linewise)) return;
    mode_ = Mode::Normal;
    if (linewise) {
        applyLinewise(op, a.r, b.r);
    } else {
        if (b.c < len(b.r)) b.c++;
        else if (b.r < lastRow() && op != '>' && op != '<') {
            // selection ends on an empty line: take its line break too
            b.r++;
            b.c = 0;
        }
        applyCharwise(op, a, b);
    }
}

void Vim::put(bool after, int count) {
    if (reg_.empty() && !regLinewise_) return;
    Line text;
    for (int i = 0; i < count; i++) {
        text += reg_;
        if (regLinewise_ && i + 1 < count) text.push_back('\n');
    }
    if (regLinewise_) {
        touch();
        int r = after ? cur_.r + 1 : cur_.r;
        lines_.insert(lines_.begin() + r, Line());
        insertAt({r, 0}, text, nullptr);
        cur_ = {r, firstNonBlank(r)};
    } else {
        Pos p = cur_;
        if (after && len(p.r) > 0) p.c++;
        Pos end;
        insertAt(p, text, &end);
        if (text.find(u'\n') == Line::npos) cur_ = {end.r, std::max(0, end.c - 1)};
        else cur_ = p;
    }
}

void Vim::joinLines(int count) {
    int joins = std::max(1, count - 1);
    for (int i = 0; i < joins && cur_.r < lastRow(); i++) {
        touch();
        Line& l = lines_[cur_.r];
        Line next = lines_[cur_.r + 1];
        size_t s = 0;
        while (s < next.size() && isSpace(next[s])) s++;
        next.erase(0, s);
        while (!l.empty() && isSpace(l.back())) l.pop_back();
        int at = (int)l.size();
        if (!l.empty() && !next.empty()) l.push_back(' ');
        l += next;
        lines_.erase(lines_.begin() + cur_.r + 1);
        cur_.c = at;
    }
}

// ------------------------------------------------------------ normal / visual mode

Vim::Request Vim::normalKey(const key::Event& ev) {
    const bool visual = mode_ == Mode::Visual || mode_ == Mode::VisualLine;
    uint16_t k = ev.code;
    message_.clear();

    if (k == key::Esc) {
        resetPending();
        if (visual) mode_ = Mode::Normal;
        return Request::None;
    }

    // --- pending prefixes that take one more key ---
    if (prefix_) {
        const uint16_t pf = prefix_;
        prefix_ = 0;
        const int count = std::max(1, count_) * std::max(1, opCount_);
        if (pf == 'r') {
            if (ev.isChar() && len(cur_.r) > 0 && cur_.c + count <= len(cur_.r)) {
                touch();
                for (int i = 0; i < count; i++) lines_[cur_.r][cur_.c + i] = (char16_t)k;
                cur_.c += count - 1;
            }
            resetPending();
            return Request::None;
        }
        if (pf == 'Z') {
            resetPending();
            if (ev.is('Z')) return Request::WriteQuit;
            if (ev.is('Q')) return Request::ForceQuit;
            return Request::None;
        }
        if (pf == 'f' || pf == 't' || pf == 'F' || pf == 'T') {
            Motion m;
            if (ev.isChar() && findChar(pf, k, count, m)) {
                lastFindKind_ = pf;
                lastFindChar_ = k;
                if (op_) applyOperator(op_, cur_, m);
                else cur_ = m.to;
                wantCol_ = cur_.c;
            }
            if (mode_ != Mode::Insert) resetPending();
            return Request::None;
        }
        if (pf == 'g') {
            if (ev.is('g')) {
                Motion m;
                motion('g', std::max(1, count), hasCount_, op_ != 0, m);
                if (op_) applyOperator(op_, cur_, m);
                else cur_ = m.to;
                wantCol_ = cur_.c;
            }
            if (mode_ != Mode::Insert) resetPending();
            return Request::None;
        }
        if (pf == 'i' || pf == 'a') {  // text object
            Pos a, b;
            if (ev.isChar() && textObject(k, pf == 'a', a, b)) {
                if (visual) {
                    mode_ = Mode::Visual;
                    anchor_ = a;
                    cur_ = {b.r, std::max(a.c, b.c - 1)};
                    return Request::None;
                }
                applyCharwise(op_, a, b);
                wantCol_ = cur_.c;
            }
            if (mode_ != Mode::Insert && !visual) resetPending();
            return Request::None;
        }
    }

    // --- counts ---
    if (ev.isChar() && k >= '0' && k <= '9' && (k != '0' || (op_ ? opCount_ : count_) > 0)) {
        int& c = op_ ? opCount_ : count_;
        if (c < 100000) c = c * 10 + (k - '0');
        hasCount_ = true;
        return Request::None;
    }
    const bool hasCount = count_ > 0 || opCount_ > 0;
    const int count = std::max(1, count_) * std::max(1, opCount_);

    // --- ctrl keys ---
    if (ev.ctrl()) {
        int delta = 0;
        if (k == 'd') delta = pageRows_ / 2;
        else if (k == 'u') delta = -pageRows_ / 2;
        else if (k == 'f') delta = pageRows_ - 1;
        else if (k == 'b') delta = -(pageRows_ - 1);
        else if (k == 'r' && !visual) {
            for (int i = 0; i < count; i++) redo();
        }
        if (delta) {
            cur_.r = std::max(0, std::min(lastRow(), cur_.r + delta));
            cur_.c = wantCol_;
        }
        if (!visual) resetPending();
        else { count_ = 0; hasCount_ = false; }
        return Request::None;
    }
    if (ev.alt()) return Request::None;
    if (k == key::PageDown || k == key::PageUp) {
        cur_.r = std::max(0, std::min(lastRow(), cur_.r + (k == key::PageDown ? pageRows_ - 1 : -(pageRows_ - 1))));
        cur_.c = wantCol_;
        return Request::None;
    }

    // --- prefixes ---
    if (k == 'g' || k == 'f' || k == 't' || k == 'F' || k == 'T' || (k == 'r' && !op_ && !visual) ||
        (k == 'Z' && !op_ && !visual) || ((k == 'i' || k == 'a') && (op_ || visual))) {
        prefix_ = k;
        return Request::None;
    }

    // --- doubled operator (dd, cc, yy, >>, <<) ---
    if (op_ && k == (uint16_t)op_) {
        const char op = op_;
        int r1 = std::min(lastRow(), cur_.r + count - 1);
        applyLinewise(op, cur_.r, r1);
        wantCol_ = cur_.c;
        if (mode_ != Mode::Insert) resetPending();
        return Request::None;
    }

    // --- motions ---
    {
        Motion m;
        uint16_t mk = k;
        bool isMotion = false;
        if (k == ';' || k == ',') {
            if (lastFindKind_) {
                uint16_t kind = lastFindKind_;
                if (k == ',') kind = (kind == 'f') ? 'F' : (kind == 'F') ? 'f' : (kind == 't') ? 'T' : 't';
                isMotion = true;
                findChar(kind, lastFindChar_, count, m);
            } else {
                isMotion = true;
            }
        } else if (k == 'n' || k == 'N' || k == '*') {
            isMotion = true;
            if (k == '*') {
                Pos a, b;
                if (textObject('w', false, a, b) && cls(lines_[a.r][a.c], false) == 1) {
                    pattern_ = lines_[a.r].substr(a.c, b.c - a.c);
                    patternForward_ = true;
                    cur_ = a;
                }
            }
            Pos before = cur_;
            for (int i = 0; i < count; i++) search(k != 'N', true);
            m.to = cur_;
            m.ok = true;
            cur_ = before;
        } else {
            // cw / cW behave like ce / cE when on a word
            if (op_ == 'c' && (k == 'w' || k == 'W') && len(cur_.r) > 0 && !isSpace(lines_[cur_.r][cur_.c])) {
                Pos p = cur_;
                p.c--;  // so that a one-letter word is its own end
                Pos e = p;
                for (int i = 0; i < count; i++) e = wordEnd(e, k == 'W');
                m.to = e;
                m.ok = true;
                m.inclusive = true;
                isMotion = true;
            } else {
                isMotion = motion(mk, count, hasCount, op_ != 0, m) || mk == 'h' || mk == 'l' || mk == 'j' || mk == 'k';
            }
        }
        if (isMotion) {
            if (m.ok) {
                if (op_) {
                    applyOperator(op_, cur_, m);
                    wantCol_ = cur_.c;
                } else {
                    cur_ = m.to;
                    const bool vertical = (k == 'j' || k == 'k' || k == key::Up || k == key::Down);
                    if (k == '$' || k == key::End) wantCol_ = 1 << 20;
                    else if (!vertical) wantCol_ = cur_.c;
                }
            }
            if (visual) {
                count_ = opCount_ = 0;
                hasCount_ = false;
            } else if (mode_ != Mode::Insert) {
                resetPending();
            }
            return Request::None;
        }
    }

    // --- visual mode commands ---
    if (visual) {
        count_ = 0;
        hasCount_ = false;
        switch (k) {
            case 'v':
                mode_ = (mode_ == Mode::Visual) ? Mode::Normal : Mode::Visual;
                break;
            case 'V':
                mode_ = (mode_ == Mode::VisualLine) ? Mode::Normal : Mode::VisualLine;
                break;
            case 'o':
                std::swap(anchor_, cur_);
                break;
            case 'd': case 'x': case key::Delete: visualOperator('d'); break;
            case 'y': visualOperator('y'); break;
            case 'c': case 's': visualOperator('c'); break;
            case '>': visualOperator('>'); break;
            case '<': visualOperator('<'); break;
            case 'D': case 'X':
                mode_ = Mode::VisualLine;
                visualOperator('d');
                break;
            case 'Y':
                mode_ = Mode::VisualLine;
                visualOperator('y');
                break;
            case 'J': {
                Pos a, b;
                bool lw;
                selection(a, b, lw);
                mode_ = Mode::Normal;
                cur_ = a;
                joinLines(b.r - a.r + 1);
                break;
            }
            case 'p': case 'P': {
                Line keep = reg_;
                bool keepLw = regLinewise_;
                visualOperator('d');
                Line deleted = reg_;
                bool delLw = regLinewise_;
                reg_ = keep;
                regLinewise_ = keepLw;
                put(false, 1);
                reg_ = deleted;
                regLinewise_ = delLw;
                break;
            }
            case ':':
                mode_ = Mode::Normal;
                break;
            default:
                break;
        }
        wantCol_ = cur_.c;
        return Request::None;
    }

    // --- operators ---
    if (k == 'd' || k == 'c' || k == 'y' || k == '>' || k == '<') {
        if (op_) {
            resetPending();  // e.g. "dc": cancel
            return Request::None;
        }
        op_ = (char)k;
        return Request::None;
    }
    if (op_) {  // unknown key after an operator cancels it
        resetPending();
        return Request::None;
    }

    // --- simple commands ---
    Request rq = Request::None;
    Motion m;
    switch (k) {
        case 'i': enterInsert(); break;
        case 'a':
            if (len(cur_.r) > 0) cur_.c++;
            enterInsert();
            break;
        case 'I':
            cur_.c = firstNonBlank(cur_.r);
            if (len(cur_.r) > 0 && isSpace(lines_[cur_.r][cur_.c])) cur_.c = len(cur_.r);
            enterInsert();
            break;
        case 'A':
            cur_.c = len(cur_.r);
            enterInsert();
            break;
        case 'o': case 'O': {
            touch();
            int ind = 0;
            const Line& l = lines_[cur_.r];
            while (ind < (int)l.size() && isSpace(l[ind])) ind++;
            Line indent = (ind < (int)l.size()) ? l.substr(0, ind) : Line();
            int r = (k == 'o') ? cur_.r + 1 : cur_.r;
            lines_.insert(lines_.begin() + r, indent);
            cur_ = {r, (int)indent.size()};
            enterInsert();
            break;
        }
        case 'x': case key::Delete:
            if (len(cur_.r) > 0) applyCharwise('d', cur_, {cur_.r, std::min(len(cur_.r), cur_.c + count)});
            break;
        case 'X':
            if (cur_.c > 0) applyCharwise('d', {cur_.r, std::max(0, cur_.c - count)}, cur_);
            break;
        case 's':
            applyCharwise('c', cur_, {cur_.r, std::min(len(cur_.r), cur_.c + count)});
            break;
        case 'S':
            applyLinewise('c', cur_.r, std::min(lastRow(), cur_.r + count - 1));
            break;
        case 'D': case 'C':
            if (motion('$', count, hasCount, true, m)) applyOperator(k == 'D' ? 'd' : 'c', cur_, m);
            if (k == 'C' && mode_ != Mode::Insert) enterInsert();
            break;
        case 'Y':
            applyLinewise('y', cur_.r, std::min(lastRow(), cur_.r + count - 1));
            break;
        case 'p': put(true, count); break;
        case 'P': put(false, count); break;
        case 'J': joinLines(count); break;
        case '~':
            for (int i = 0; i < count && cur_.c < len(cur_.r); i++) {
                char16_t& ch = lines_[cur_.r][cur_.c];
                char16_t n = ch;
                if (ch >= 'a' && ch <= 'z') n = ch - 32;
                else if (ch >= 'A' && ch <= 'Z') n = ch + 32;
                if (n != ch) {
                    touch();
                    lines_[cur_.r][cur_.c] = n;
                }
                cur_.c++;
            }
            break;
        case 'u':
            commitUndo();
            for (int i = 0; i < count; i++) undo();
            break;
        case '.':
            if (!lastChange_.empty() && !replaying_) {
                resetPending();
                replaying_ = true;
                std::vector<key::Event> keys = lastChange_;
                for (auto& e : keys) handle(e);
                if (mode_ == Mode::Insert) leaveInsert();
                replaying_ = false;
                recTick_ = tick_;  // "." itself must not become the last change
            }
            break;
        case 'v':
            anchor_ = cur_;
            mode_ = Mode::Visual;
            break;
        case 'V':
            anchor_ = cur_;
            mode_ = Mode::VisualLine;
            break;
        case ':':
            cmd_.clear();
            mode_ = Mode::Command;
            break;
        case '/': case '?':
            cmd_.clear();
            searchForward_ = (k == '/');
            mode_ = Mode::Search;
            break;
        default:
            break;
    }
    if (mode_ == Mode::Normal) wantCol_ = cur_.c;
    count_ = opCount_ = 0;
    hasCount_ = false;
    op_ = 0;
    return rq;
}
