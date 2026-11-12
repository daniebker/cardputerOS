# Changelog

All notable changes to cardputerOS are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the
project uses [Semantic Versioning](https://semver.org/).

## [0.3.0] - 2026-11-12

### Changed

- The home screen is a grid of app icons instead of a list. Icons are drawn
  from shapes (no image files); running apps show a green dot, and the
  selected app's description is shown at the bottom.

### Fixed

- The small (4x6) terminal font showed no readable text: its characters were
  drawn above their row and cut off.

## [0.2.0] - 2026-11-12

### Added

- **Ember Crown**, a roguelike: eight generated dungeon levels, field of view,
  ten kinds of monsters, potions, scrolls, weapons and armor, permadeath, and a
  saved run that survives reboots. The game rules are tested on the host by a
  bot that plays hundreds of games.

## [0.1.0] - 2026-11-12

First release for the M5Stack Cardputer ADV with an external 2.8" ILI9341 display.

### Added

- **OS core**
  - Launcher with app switching; apps keep running in the background
    (`opt`+`` ` `` or the G0 button returns to the launcher, `x` closes an app).
  - App framework: one file per app, registered with `OS_REGISTER_APP`.
  - UI toolkit: lists, text prompts, confirmations, alerts, choice dialogs.
  - Status (app, Wi-Fi, IP, battery, clock) on the Cardputer's built-in screen,
    leaving the whole external display to the apps.
  - Keyboard driver with key repeat and an `fn` layer (Esc, arrows, Home/End,
    Page Up/Down, F1–F10, Delete).
  - Wi-Fi with saved networks, background auto-connect and NTP time.
  - Settings: Wi-Fi, 180° screen rotation, terminal font, time zone, About
    (including the reasons for the last boots).
  - Dracula colour scheme throughout, defined in `src/os/theme.h`.
  - `opt`+`r` resets and redraws the external display.
- **SSH**
  - Saved hosts, password and keyboard-interactive login.
  - On-device Ed25519 key pair; `opt`+`k` installs it on the connected host;
    the public key is also written to the SD card if one is inserted.
  - Trust-on-first-use host key checking with a warning when a key changes.
  - xterm-compatible terminal (256 colours, line drawing, alternate screen,
    scrollback) with four font sizes, 80×40 down to 40×15.
- **Notes**
  - Markdown notes with a vi-style editor: motions, counts, operators, text
    objects, visual mode, search, undo/redo and `.` repeat.
  - Word wrap and Markdown highlighting (headings, lists, checkboxes, quotes,
    code).
  - Two-way sync of `.md` files with a WebDAV folder (e.g. Nextcloud), with
    conflict copies and trust-on-first-use HTTPS certificate pinning.
- **Sudoku**
  - Generated puzzles with exactly one solution in three difficulties.
  - Pencil marks, automatic marks, undo, hints, conflict highlighting,
    checking, timer, best times; the game in progress survives reboots.
- **Development**
  - Host unit tests (`pio test -e native`) for the terminal emulator, the
    editor engine and layout, the WebDAV/HTTP code and the Sudoku engine.
  - Debug build (`-DSCREENSHOT_DEBUG`) that renders off-screen and accepts
    keystrokes and screenshot requests over USB serial.

### Hardware notes

- Power the display (VCC and LED) from EXT pin 6 (5VOUT). Pin 2 (5VIN) only
  carries 5 V while USB is connected, so the display does not start on battery.
