#include "ui.h"

#include <algorithm>

#include "kernel.h"
#include "theme.h"
#include "vterm.h"

namespace ui {

namespace {
constexpr int kRowH = 22;
constexpr int kPad = 8;

// Word-wrap `text` to `maxW` pixels with the font currently selected on `g`.
std::vector<std::string> wrap(os::Gfx& g, const std::string& text, int maxW) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string para = text.substr(pos, nl - pos);
        pos = nl + 1;
        std::string line;
        size_t i = 0;
        while (i < para.size()) {
            size_t sp = para.find(' ', i);
            if (sp == std::string::npos) sp = para.size();
            std::string word = para.substr(i, sp - i);
            i = sp + 1;
            std::string candidate = line.empty() ? word : line + " " + word;
            if (g.textWidth(candidate.c_str()) <= maxW) {
                line = candidate;
                continue;
            }
            if (!line.empty()) lines.push_back(line);
            line.clear();
            // hard-break words that are wider than a whole line (fingerprints, URLs)
            while (g.textWidth(word.c_str()) > maxW && word.size() > 1) {
                size_t n = word.size() - 1;
                while (n > 1 && g.textWidth(word.substr(0, n).c_str()) > maxW) n--;
                lines.push_back(word.substr(0, n));
                word = word.substr(n);
            }
            line = word;
        }
        lines.push_back(line);
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

// Draws a dialog frame centred in the content area and returns its inner rect.
os::Rect dialogBox(os::Gfx& g, const char* title, int innerH, uint32_t titleColor = theme::accent) {
    os::Rect c = os::content();
    g.setFont(theme::fontUi());
    const int titleH = g.fontHeight() + 8;
    int w = c.w - 20;
    int h = std::min(titleH + innerH + 2 * kPad, c.h - 8);
    int x = c.x + (c.w - w) / 2;
    int y = c.y + (c.h - h) / 2;
    g.startWrite();
    g.fillRect(x, y, w, h, theme::panel);
    g.drawRect(x, y, w, h, theme::border);
    g.fillRect(x + 1, y + 1, w - 2, titleH - 1, theme::select);
    g.setTextDatum(textdatum_t::top_left);
    g.setTextColor(titleColor);
    drawClipped(g, title, x + kPad, y + 4, w - 2 * kPad);
    g.endWrite();
    return {x + kPad, y + titleH + kPad, w - 2 * kPad, h - titleH - 2 * kPad};
}

void hintLine(os::Gfx& g, const os::Rect& inner, const char* hint) {
    g.setFont(theme::fontSmall());
    g.setTextColor(theme::dim);
    g.setTextDatum(textdatum_t::top_left);
    g.drawString(hint, inner.x, inner.y + inner.h - g.fontHeight());
}
}  // namespace

bool isUp(const key::Event& ev) { return ev.is(key::Up) || ev.is(';') || ev.is('k'); }
bool isDown(const key::Event& ev) { return ev.is(key::Down) || ev.is('.') || ev.is('j'); }

void drawClipped(os::Gfx& g, const std::string& text, int x, int y, int maxWidth) {
    if (maxWidth <= 0) return;
    if (g.textWidth(text.c_str()) <= maxWidth) {
        g.drawString(text.c_str(), x, y);
        return;
    }
    std::string t = text;
    while (!t.empty() && g.textWidth((t + "..").c_str()) > maxWidth) t.pop_back();
    g.drawString((t + "..").c_str(), x, y);
}

std::string displayable(const std::string& utf8) {
    std::string out;
    size_t i = 0;
    while (i < utf8.size()) {
        const uint8_t c = (uint8_t)utf8[i];
        uint32_t cp = c;
        int n = 1;
        if (c >= 0xF0) { cp = c & 0x07; n = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; n = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; n = 2; }
        for (int k = 1; k < n && i + k < utf8.size(); k++) cp = (cp << 6) | ((uint8_t)utf8[i + k] & 0x3F);
        i += n;
        if (cp < 0x20) out.push_back(' ');
        else if (cp < 0x7F) out.push_back((char)cp);
        else {
            const uint8_t g = VTerm::glyphFor(cp);
            if (g == 0) continue;  // invisible (joiners, variation selectors)
            out.push_back(g < 0x7F ? (char)g : '?');
        }
    }
    return out;
}

os::Rect footer(os::Gfx& g, const os::Rect& area, const char* hint) {
    g.setFont(theme::fontSmall());
    const int h = g.fontHeight() + 6;
    g.fillRect(area.x, area.y + area.h - h, area.w, h, theme::panel);
    g.setTextColor(theme::dim);
    g.setTextDatum(textdatum_t::top_left);
    drawClipped(g, hint, area.x + 6, area.y + area.h - h + 3, area.w - 12);
    return {area.x, area.y, area.w, area.h - h};
}

os::Rect header(os::Gfx& g, const os::Rect& area, const char* title) {
    g.setFont(theme::fontUi());
    const int h = g.fontHeight() + 8;
    g.fillRect(area.x, area.y, area.w, h, theme::bg);
    g.setTextColor(theme::accent);
    g.setTextDatum(textdatum_t::top_left);
    drawClipped(g, title, area.x + 8, area.y + 5, area.w - 16);
    g.drawFastHLine(area.x + 6, area.y + h - 1, area.w - 12, theme::border);
    return {area.x, area.y + h, area.w, area.h - h};
}

// ---------------------------------------------------------------- List

int List::visibleRows(const os::Rect& r) const { return std::max(1, r.h / kRowH); }

void List::clampSelection() {
    if (selected >= (int)items.size()) selected = (int)items.size() - 1;
    if (selected < 0) selected = 0;
}

void List::drawRow(os::Gfx& g, const os::Rect& r, int index) {
    const int rowIdx = index - top_;
    if (rowIdx < 0 || rowIdx >= visibleRows(r) || index >= (int)items.size()) return;
    const int y = r.y + rowIdx * kRowH;
    const bool sel = index == selected;
    const ListItem& it = items[index];
    g.fillRect(r.x, y, r.w, kRowH, sel ? theme::select : theme::bg);
    if (sel) g.fillRect(r.x, y, 3, kRowH, theme::accent);
    g.setFont(theme::fontUi());
    g.setTextDatum(textdatum_t::top_left);
    const int ty = y + (kRowH - g.fontHeight()) / 2;
    int detailW = 0;
    if (!it.detail.empty()) {
        g.setFont(theme::fontSmall());
        detailW = std::min<int>(g.textWidth(it.detail.c_str()), r.w / 2);
        g.setTextColor(theme::dim);
        drawClipped(g, it.detail, r.x + r.w - detailW - 8, y + (kRowH - g.fontHeight()) / 2, detailW);
        detailW += 14;
        g.setFont(theme::fontUi());
    }
    g.setTextColor(it.color ? it.color : theme::text);
    drawClipped(g, it.label, r.x + 10, ty, r.w - 18 - detailW);
}

void List::draw(os::Gfx& g, const os::Rect& r) {
    clampSelection();
    const int rows = visibleRows(r);
    if (selected < top_) top_ = selected;
    if (selected >= top_ + rows) top_ = selected - rows + 1;
    if (top_ > std::max(0, (int)items.size() - rows)) top_ = std::max(0, (int)items.size() - rows);
    g.startWrite();
    g.fillRect(r.x, r.y, r.w, r.h, theme::bg);
    if (items.empty()) {
        g.setFont(theme::fontUi());
        g.setTextColor(theme::dim);
        g.setTextDatum(textdatum_t::top_left);
        g.drawString(emptyText, r.x + 10, r.y + 6);
    }
    for (int i = top_; i < top_ + rows && i < (int)items.size(); i++) drawRow(g, r, i);
    // scroll markers
    if (top_ > 0) g.fillTriangle(r.x + r.w - 9, r.y + 6, r.x + r.w - 3, r.y + 6, r.x + r.w - 6, r.y + 2, theme::dim);
    if (top_ + rows < (int)items.size()) {
        const int by = r.y + rows * kRowH - 3;
        g.fillTriangle(r.x + r.w - 9, by - 4, r.x + r.w - 3, by - 4, r.x + r.w - 6, by, theme::dim);
    }
    g.endWrite();
}

void List::update(os::Gfx& g, const os::Rect& r, std::vector<ListItem> newItems, int newSelected) {
    std::vector<ListItem> old;
    old.swap(items);
    items.swap(newItems);
    const int oldSelected = selected;
    selected = newSelected;
    clampSelection();
    const int rows = visibleRows(r);
    int top = top_;
    if (selected < top) top = selected;
    if (selected >= top + rows) top = selected - rows + 1;
    if (top > std::max(0, (int)items.size() - rows)) top = std::max(0, (int)items.size() - rows);
    const bool moreAbove = top_ > 0, moreBelow = top_ + rows < (int)old.size();
    const bool scrolled = top != top_ || old.empty() != items.empty() || moreAbove != (top > 0) ||
                          moreBelow != (top + rows < (int)items.size());
    if (scrolled) {
        draw(g, r);
        return;
    }
    g.startWrite();
    for (int i = top_; i < top_ + rows; i++) {
        const bool inOld = i < (int)old.size(), inNew = i < (int)items.size();
        if (!inOld && !inNew) break;
        const bool selMoved = oldSelected != selected && (i == selected || i == oldSelected);
        if (inNew && inOld && old[i] == items[i] && !selMoved) continue;
        if (inNew) drawRow(g, r, i);
        else g.fillRect(r.x, r.y + (i - top_) * kRowH, r.w, kRowH, theme::bg);
    }
    if (top_ > 0) g.fillTriangle(r.x + r.w - 9, r.y + 6, r.x + r.w - 3, r.y + 6, r.x + r.w - 6, r.y + 2, theme::dim);
    if (top_ + rows < (int)items.size()) {
        const int by = r.y + rows * kRowH - 3;
        g.fillTriangle(r.x + r.w - 9, by - 4, r.x + r.w - 3, by - 4, r.x + r.w - 6, by, theme::dim);
    }
    g.endWrite();
}

List::Result List::onKey(os::Gfx& g, const os::Rect& r, const key::Event& ev) {
    if (items.empty()) return None;
    const int rows = visibleRows(r);
    const int old = selected;
    if (ev.is(key::Enter)) return Activated;
    if (isUp(ev)) selected--;
    else if (isDown(ev)) selected++;
    else if (ev.is(key::PageUp)) selected -= rows;
    else if (ev.is(key::PageDown)) selected += rows;
    else if (ev.is(key::Home)) selected = 0;
    else if (ev.is(key::End)) selected = (int)items.size() - 1;
    else return None;
    clampSelection();
    if (selected == old) return None;
    if (selected < top_ || selected >= top_ + rows) {
        draw(g, r);
    } else {
        g.startWrite();
        drawRow(g, r, old);
        drawRow(g, r, selected);
        g.endWrite();
    }
    return Moved;
}

// ---------------------------------------------------------------- dialogs

bool prompt(const char* title, std::string& value, bool secret, const char* hint) {
    os::Gfx& g = os::gfx();
    g.setFont(theme::fontUi());
    const int fieldH = g.fontHeight() + 10;
    g.setFont(theme::fontSmall());
    const int smallH = g.fontHeight();
    const int innerH = fieldH + 8 + smallH + (hint ? smallH + 6 : 0);
    os::Rect in = dialogBox(g, title, innerH);
    int y = in.y;
    if (hint) {
        g.setFont(theme::fontSmall());
        g.setTextColor(theme::dim);
        drawClipped(g, hint, in.x, y, in.w);
        y += smallH + 6;
    }
    hintLine(g, in, secret ? "Enter: OK   Esc (fn+`): cancel   Tab: show/hide" : "Enter: OK   Esc (fn+`): cancel");

    std::string text = value;
    bool masked = secret;
    bool dirty = true;
    for (;;) {
        if (dirty) {
            dirty = false;
            g.startWrite();
            g.fillRect(in.x, y, in.w, fieldH, theme::bg);
            g.drawRect(in.x, y, in.w, fieldH, theme::accent);
            g.setFont(theme::fontUi());
            g.setTextDatum(textdatum_t::top_left);
            g.setTextColor(theme::text);
            std::string shown = masked ? std::string(text.size(), '*') : text;
            shown += "_";
            // keep the end of the text visible
            const int maxW = in.w - 12;
            size_t start = 0;
            while (start < shown.size() && g.textWidth(shown.c_str() + start) > maxW) start++;
            g.drawString(shown.c_str() + start, in.x + 6, y + 5);
            g.endWrite();
        }
        key::Event ev;
        if (!os::modalPoll(ev)) continue;
        if (ev.is(key::Enter)) {
            value = text;
            return true;
        }
        if (ev.code == key::Esc) return false;
        if (ev.is(key::Tab) && secret) {
            masked = !masked;
            dirty = true;
        } else if (ev.code == key::Backspace) {
            if (ev.ctrl()) text.clear();
            else if (!text.empty()) text.pop_back();
            dirty = true;
        } else if (ev.isCtrl('u')) {
            text.clear();
            dirty = true;
        } else if (ev.isChar() && text.size() < 200) {
            text.push_back((char)ev.code);
            dirty = true;
        }
    }
}

namespace {
// Shared body for alert / confirm. Returns the inner rect.
os::Rect messageBox(const char* title, const std::string& message, const char* hint, uint32_t titleColor) {
    os::Gfx& g = os::gfx();
    os::Rect c = os::content();
    g.setFont(theme::fontUi());
    const int lineH = g.fontHeight() + 2;
    auto lines = wrap(g, message, c.w - 20 - 2 * kPad);
    g.setFont(theme::fontSmall());
    const int smallH = g.fontHeight();
    const int maxLines = std::max(1, (c.h - 60 - smallH) / lineH);
    if ((int)lines.size() > maxLines) lines.resize(maxLines);
    os::Rect in = dialogBox(g, title, (int)lines.size() * lineH + 8 + smallH, titleColor);
    g.setFont(theme::fontUi());
    g.setTextColor(theme::text);
    g.setTextDatum(textdatum_t::top_left);
    int y = in.y;
    for (auto& l : lines) {
        g.drawString(l.c_str(), in.x, y);
        y += lineH;
    }
    hintLine(g, in, hint);
    return in;
}
}  // namespace

bool confirm(const char* title, const std::string& message, bool defaultYes) {
    messageBox(title, message, defaultYes ? "Enter / y: yes     n / Esc (fn+`): no" : "y: yes     n / Enter / Esc: no",
               theme::warn);
    for (;;) {
        key::Event ev;
        if (!os::modalPoll(ev)) continue;
        if (ev.is('y') || ev.is('Y')) return true;
        if (ev.is('n') || ev.is('N') || ev.code == key::Esc) return false;
        if (ev.is(key::Enter)) return defaultYes;
    }
}

void alert(const char* title, const std::string& message) {
    messageBox(title, message, "Enter: OK", theme::accent);
    for (;;) {
        key::Event ev;
        if (!os::modalPoll(ev)) continue;
        if (ev.is(key::Enter) || ev.code == key::Esc) return;
    }
}

int choose(const char* title, const std::vector<std::string>& items, int initial) {
    os::Gfx& g = os::gfx();
    os::Rect c = os::content();
    g.setFont(theme::fontSmall());
    const int smallH = g.fontHeight();
    const int maxRows = std::max(1, (c.h - 70 - smallH) / kRowH);
    const int rows = std::min<int>(maxRows, std::max<int>(1, items.size()));
    os::Rect in = dialogBox(g, title, rows * kRowH + 6 + smallH);
    hintLine(g, in, "Enter: select   Esc (fn+`): cancel");
    List list;
    for (auto& s : items) list.items.push_back({s, "", 0});
    list.selected = initial;
    os::Rect lr{in.x, in.y, in.w, rows * kRowH};
    list.draw(g, lr);
    for (;;) {
        key::Event ev;
        if (!os::modalPoll(ev)) continue;
        if (ev.code == key::Esc) return -1;
        if (list.onKey(g, lr, ev) == List::Activated) return list.selected;
    }
}

void busy(const std::string& message) {
    os::Gfx& g = os::gfx();
    g.setFont(theme::fontUi());
    os::Rect in = dialogBox(g, "Please wait", g.fontHeight() + 4);
    g.setFont(theme::fontUi());
    g.setTextColor(theme::text);
    g.setTextDatum(textdatum_t::top_left);
    drawClipped(g, message, in.x, in.y, in.w);
}

}  // namespace ui
