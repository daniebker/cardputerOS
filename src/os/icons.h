// App icons for the home screen, drawn from simple shapes so they need no image
// data. Each function draws into the square (x, y, size, size).
#pragma once
#include "app.h"

namespace icons {

// fg: the app's colour, bg: the colour behind the icon (for cut-outs).
using DrawFn = void (*)(os::Gfx& g, int x, int y, int size, uint32_t fg, uint32_t bg);

void terminal(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t bg);
void note(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t bg);
void gear(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t bg);
void sudoku(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t bg);
void crown(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t bg);
void mesh(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t bg);
void journal(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t bg);
// Fallback: a rounded square with the first letter of the name.
void letter(os::Gfx& g, int x, int y, int s, uint32_t fg, uint32_t bg, char c);

}  // namespace icons
