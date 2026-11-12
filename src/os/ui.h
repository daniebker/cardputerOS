// Small UI toolkit: a list widget and blocking dialogs.
//
// The dialogs draw over the app's content area and block until dismissed
// (OS services keep running). The caller must repaint itself afterwards.
#pragma once
#include <string>
#include <vector>

#include "app.h"

namespace ui {

struct ListItem {
    std::string label;
    std::string detail;       // right-aligned secondary text
    uint32_t color = 0;       // label colour, 0 = default
};

class List {
public:
    enum Result { None, Moved, Activated };

    std::vector<ListItem> items;
    int selected = 0;
    const char* emptyText = "(nothing here)";

    void draw(os::Gfx& g, const os::Rect& r);  // full repaint
    // Handles navigation keys and repaints what changed.
    Result onKey(os::Gfx& g, const os::Rect& r, const key::Event& ev);
    void clampSelection();

private:
    void drawRow(os::Gfx& g, const os::Rect& r, int index);
    int visibleRows(const os::Rect& r) const;
    int top_ = 0;
};

// True if the event should be treated as "move up" / "move down" in a menu.
bool isUp(const key::Event& ev);
bool isDown(const key::Event& ev);

// Draws a one-line key hint at the bottom of `area` and returns the area above it.
os::Rect footer(os::Gfx& g, const os::Rect& area, const char* hint);
// Draws a title line at the top of `area` and returns the area below it.
os::Rect header(os::Gfx& g, const os::Rect& area, const char* title);
// Draws `text` at x,y cut to maxWidth pixels (uses the current font / colour).
void drawClipped(os::Gfx& g, const std::string& text, int x, int y, int maxWidth);

// --- blocking dialogs ---
// Text entry. Returns false if cancelled. `secret` masks the input (Tab toggles).
bool prompt(const char* title, std::string& value, bool secret = false, const char* hint = nullptr);
// Yes/no question: y = yes, n or Esc = no, Enter picks the default.
bool confirm(const char* title, const std::string& message, bool defaultYes = false);
void alert(const char* title, const std::string& message);
// Pick one of `items`; returns the index or -1.
int choose(const char* title, const std::vector<std::string>& items, int initial = 0);
// Non-blocking "please wait" box.
void busy(const std::string& message);

}  // namespace ui
