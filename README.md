# cardputerOS

A small operating system for the **M5Stack Cardputer ADV** that uses an external
**2.8" ILI9341 (320x240, SPI)** display as its main screen. It has a launcher, a
status bar, background apps and a built-in SSH client. The built-in 1.14" screen
shows a short status summary.

## Wiring

The display connects to the 14-pin EXT header. Defaults (edit `include/board_config.h`
if yours differs):

| ILI9341 pin | EXT header pin | GPIO |
|-------------|----------------|------|
| VCC         | 6 (5VOUT)      |      |
| GND         | 4              |      |
| CS          | 13             | G5   |
| RESET       | 1              | G3   |
| DC / RS     | 5              | G6   |
| SDI (MOSI)  | 9              | G14  |
| SCK         | 7              | G40  |
| LED         | 6 (5VOUT), shared with VCC |      |
| SDO (MISO)  | not connected  |      |

Power the display from **pin 6 (5VOUT)**, not pin 2 (5VIN): pin 2 is the Cardputer's
power *input* and only carries 5 V while USB is plugged in, so on battery the display
does not start (black, flickering screen). The display module must have its own 3.3 V
regulator (2.8" ILI9341 boards normally do).

Touch pins (T_*) are not used.
 If the picture is noisy lower `EXT_TFT_FREQ`; if colours
are wrong see `EXT_TFT_BGR` / `EXT_TFT_INVERT` in the same file.

## Build and flash

```
pio run -t upload          # build + flash over USB
pio test -e native         # run the host-side unit tests
```

If the Cardputer does not show up as a serial port, hold **G0** while switching it on
to enter download mode.

## Keys

| Keys | Action |
|------|--------|
| `opt` + `` ` ``, or the **G0** button | Back to the launcher (the app keeps running) |
| `fn` + `` ` `` | Esc |
| `fn` + `;` `.` `,` `/` | Up / Down / Left / Right |
| `fn` + `1`…`0` | F1…F10 |
| `fn` + Backspace | Delete |
| `fn` + `-` / `=` | Home / End |
| `fn` + `[` / `]` | Page Up / Page Down |
| `ctrl`, `alt` + key | As on a PC keyboard (alt sends Esc-prefix in the terminal) |
| `opt` + `r` | Reset and redraw the external display (if it ever shows garbage or stays white) |

In menus `;` and `.` (or `k` / `j`) move the selection without holding `fn`.

SSH terminal:

| Keys | Action |
|------|--------|
| `opt` + `;` / `.` | Scroll back / forward through history |
| `opt` + `f` | Cycle font size: small 4x6 (80x37), normal 6x8 (53x28), medium 8x10 (40x22, default), large 8x16 (40x14) |
| `opt` + `x` | Disconnect |
| `opt` + `k` | Install this device's SSH key on the host |

## SSH

1. Launcher → **Settings** → **Wi-Fi**, pick a network.
2. Launcher → **SSH** → **+ New host**.

Host keys are checked trust-on-first-use: the fingerprint is shown the first time and a
warning appears if it ever changes. Passwords are optional; a saved password is stored
unencrypted in the device's flash, so leave it empty to be asked on every connection.

**Key authentication.** In the SSH host list press `k` to create the device's own Ed25519
key (it is generated on the device and the private key never leaves it). The screen shows
the public key and fingerprint. If a microSD card is inserted, the public key is also
written to `/cardputerOS/id_ed25519.pub` on the card (`s` on the key screen copies it
again, e.g. after inserting a card). The private key stays in the device's internal
flash and is never written to the card. The public key is also printed on the USB
serial console (`pio device monitor`). To install it on a host, log in once with the password
and press `opt`+`k` in the terminal: the key is appended to `~/.ssh/authorized_keys` there.
From then on that host lets you in without a password. `g` on the key screen replaces the
key with a new one.

Do not run `pio run -t uploadfs`: it rewrites the whole internal file system, which
erases the device key and all notes.

## Notes

Launcher → **Notes**. `n` starts a new note, Enter opens one, `d` deletes, `s` syncs,
`a` sets up the sync account. The first line of a note is its title.

The editor uses vi keys: `i a o O` to insert, `Esc` (`fn`+`` ` ``, or `ctrl`+`[`) back to
normal mode, `:w` save, `:wq` / `ZZ` save and close, `:q!` discard. Motions, counts,
`d c y p`, text objects (`ciw`, `di"`), visual mode (`v`, `V`), `/` search, `u` /
`ctrl`+`r` undo/redo and `.` repeat work. `opt`+`f` switches between a small and a large
font. Text is saved automatically when you leave for the launcher.

Notes are Markdown. A new note starts with a `# ` heading; the first line is the title.
The editor colours headings, lists and checkboxes, quotes, code blocks and `inline code`.

**WebDAV sync.** Press `a` and enter the address of a WebDAV folder, the user name and
password. For Nextcloud that is `https://HOST/remote.php/dav/files/USER/Notes/` (use an
app password if you have two-factor login); this is the folder the Nextcloud Notes app
uses too. If the folder does not exist yet, the app offers to create it. Each note is one
`.md` file named after its title when it is first uploaded (`Groceries.md`,
`Groceries 2.md` if the name is taken); the file name does not change when you edit the
title later. Other files in the folder are ignored, and `.md` files added by other apps
show up as notes. Sync runs when the app is first opened, after saving or deleting a
note, and on `s`. If a note was changed both here and on the server, the server version
wins and yours is kept as a separate "(conflict copy)" note.

HTTPS certificates are not checked against a CA list; like SSH host keys, the server's
certificate fingerprint is shown on first use and you are warned if it later changes
(which also happens on every certificate renewal). The password is stored unencrypted
in flash.

## Sudoku

Launcher → **Sudoku**. Every puzzle is generated on the device and has exactly one
solution: Easy (about 40 given digits), Medium (about 32) or Hard (as few as possible,
usually 26–30). The game in progress is saved, so it survives closing the app and
rebooting; undo history only lasts for the current session.

| Keys | Action |
|------|--------|
| arrows, `h` `j` `k` `l` or `w` `a` `s` `d` | Move (wraps around the edges) |
| `1`–`9` | Enter a digit (or a pencil mark in pencil mode) |
| `shift` + `1`–`9` | Toggle a pencil mark directly |
| `0`, `x`, space, Backspace | Clear the cell |
| `p` or Tab | Switch between pen and pencil |
| `m` | Fill in all pencil marks the rules allow |
| `u` | Undo |
| `?` | Hint: reveal the selected cell (or the first wrong/empty one) |
| `c` | Check: mark wrong digits in red |
| `N` | New game (asks for the difficulty) |

Digits that break a rule turn red straight away, the selected cell's row, column and box
are shaded, and other cells with the same digit (and matching pencil marks) light up. The
side panel shows the time and which digits are not finished yet. Best times (without
hints) are kept per difficulty.

## Ember Crown (roguelike)

Launcher → **Ember Crown**. Explore eight randomly generated dungeon levels, fight what
lives there, and take the Ember Crown (`*`) from its guardian on depth 8. Death is
permanent; the run in progress is saved when you leave the app (and every 50 turns).

| Keys | Action |
|------|--------|
| `h` `j` `k` `l`, or `;` `.` `,` `/`, or the arrows | Move; walk into a monster to attack |
| `y` `u` `b` `n` | Move diagonally |
| `g` | Pick up (Enter also works) |
| `i` / `d` | Use or equip an item / drop an item |
| `s` or space | Rest one turn |
| `>` or Enter | Take the stairs down |
| `?` | Help |
| `N` | New game |

`!` potions, `?` scrolls, `)` weapons, `[` armor, `$` gold. Hit points come back slowly
over time, but resting is never completely safe: now and then something comes looking
for you.

## Layout

```
include/board_config.h   pins and panel options
lib/core                 key event type (portable)
lib/vterm                terminal emulator (portable, unit tested on the host)
src/hal                  ILI9341 display driver, keyboard driver
src/os                   kernel (launcher, status bar, app lifecycle), UI toolkit,
                         Wi-Fi, storage, terminal renderer
lib/vim                  vi-style editor engine (portable, unit tested on the host)
lib/dav                  WebDAV / HTTP parsing (portable, unit tested)
lib/sudoku               Sudoku generator and solver (portable, unit tested)
lib/rogue                roguelike game rules (portable, unit tested with a bot player)
src/apps                 apps: settings, ssh, notes, sudoku, rogue
test/                    host-side unit tests
```

## Adding an app

Create a `.cpp` under `src/apps`, derive from `os::App` (see `src/os/app.h`) and register it:

```cpp
class NotesApp : public os::App {
    void onResume() override { /* paint into os::content() using os::gfx() */ }
    void onKey(const key::Event& ev) override { /* ... */ }
};
OS_REGISTER_APP(NotesApp, "Notes", "Vim-style notes", 0xF0B040);
```

It appears in the launcher automatically. `ui::List`, `ui::prompt`, `ui::confirm` and
`os::TermView` are available as building blocks.
