# cardputerOS

A small operating system for the **M5Stack Cardputer ADV** that uses an external
**2.8" ILI9341 (320x240, SPI)** display as its main screen. It has a launcher, a
status bar, background apps and a built-in SSH client. The built-in 1.14" screen
shows a short status summary.

**[Install it from the browser with the web flasher](https://daniebker.github.io/cardputerOS/)**
(Chrome or Edge on a desktop computer, USB cable, nothing else to install).

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
| LED         | 12             | G13  |
| SDO (MISO)  | not connected  |      |

Power the display from **pin 6 (5VOUT)**, not pin 2 (5VIN): pin 2 is the Cardputer's
power *input* and only carries 5 V while USB is plugged in, so on battery the display
does not start (black, flickering screen). The display module must have its own 3.3 V
regulator (2.8" ILI9341 boards normally do).

Touch pins (T_*) are not used.
 If the picture is noisy lower `EXT_TFT_FREQ`; if colours
are wrong see `EXT_TFT_BGR` / `EXT_TFT_INVERT` in the same file.

## Build and flash

The quickest way to install a released version is the
[web flasher](https://daniebker.github.io/cardputerOS/): connect the Cardputer over USB, click
**Connect and install**, pick its serial port and choose **Install cardputerOS**. Leave
*Erase device* unticked to keep notes, keys and settings. To build it yourself:

```
pio run -t upload          # build + flash over USB
pio test -e native         # run the host-side unit tests
```

If the Cardputer does not show up as a serial port, hold **G0** while switching it on
to enter download mode.

**CI and releases.** GitHub Actions (`.github/workflows/build.yml`) runs the unit tests
and builds the firmware on every push to a branch and on pull requests; the images are
attached to the run as the `firmware` artifact. A release is only created when a version
tag is pushed:

```
# bump kVersion in src/os/kernel.h and add a "## [x.y.z]" section to CHANGELOG.md, then
git tag -a v0.6.0 -m "cardputerOS 0.6.0" && git push origin main v0.6.0
```

The tag must match `kVersion` and have a CHANGELOG section, which becomes the release
notes, and the [web flasher](https://daniebker.github.io/cardputerOS/) on GitHub Pages is
updated to it. Each release also has `cardputerOS-vX.Y.Z-app.bin` (flash at `0x10000` to
update; everything is kept) and `-full.bin` (flash at `0x0` for a first install with
`esptool.py --chip esp32s3 write_flash 0x0 ...`; it keeps notes and keys but resets the
settings).

The flasher page is `web/index.html`. Before the first release, enable Pages once in the
repository: **Settings → Pages → Source: GitHub Actions**. To try the page locally, put a
`manifest.json` and the firmware next to it and serve the folder on `localhost` (Web
Serial only works over HTTPS or on localhost).

## Keys

| Keys | Action |
|------|--------|
| `opt` + `` ` ``, or the **G0** button | Back to the home screen (the app keeps running) |
| `fn` + `` ` `` | Esc |
| `fn` + `;` `.` `,` `/` | Up / Down / Left / Right |
| `fn` + `1`…`0` | F1…F10 |
| `fn` + Backspace | Delete |
| `fn` + `-` / `=` | Home / End |
| `fn` + `[` / `]` | Page Up / Page Down |
| `ctrl`, `alt` + key | As on a PC keyboard (alt sends Esc-prefix in the terminal) |
| `opt` + `r` | Reset and redraw the external display (if it ever shows garbage or stays white) |

On the home screen the arrows (or `;` `.` `,` `/`, or `h` `j` `k` `l`) move between app
icons, Enter opens one, and `x` closes a running app (marked with a green dot). The games
are grouped in the **Games** folder: Enter opens it, and Esc, Backspace or `opt`+`` ` ``
goes back (leaving a game returns to the folder). In menus
`;` and `.` (or `k` / `j`) move the selection without holding `fn`.

SSH terminal:

| Keys | Action |
|------|--------|
| `opt` + `;` / `.` | Scroll back / forward through history |
| `opt` + `f` | Cycle font size: small 4x6 (80x37), normal 6x8 (53x28), medium 8x10 (40x22, default), large 8x16 (40x14) |
| `opt` + `x` | Disconnect |
| `opt` + `k` | Install this device's SSH key on the host |

## Passphrase and encryption

Saved secrets are stored encrypted: Wi-Fi passwords, SSH hosts and their passwords,
the WebDAV passwords of Notes, Journal and Tasks, the Calendar links and passwords,
and the SSH private key. On the first start
with this version you choose a passphrase (at least 6 characters); any secrets already
stored are encrypted with it right away. From then on the passphrase is asked for each
time the Cardputer starts. Waking from sleep does not ask again. Until it is entered
nothing can read the secrets, so Wi-Fi only starts after unlocking.

After 3 wrong passphrases in a row the unlock prompt makes you wait 30 seconds, and
each further wrong one doubles the wait, up to an hour. The count is kept across
restarts and saved before each check, so restarting or pulling the power does not
help: a restart starts the wait over. Esc during the wait still lets you delete the
secrets and start fresh. Changing the passphrase in Settings counts in the same way.

**Settings → Passphrase** changes it. If you forget it, press Esc at the unlock prompt:
the encrypted secrets and the SSH key are deleted (they cannot be recovered) and you
choose a new passphrase. Notes, journal entries and other settings are not affected.

How it works: a random 256-bit key encrypts each secret with AES-256-GCM. That key is
stored wrapped with a key derived from the passphrase (PBKDF2-SHA256, 30,000 rounds),
so a copy of the flash memory alone reveals none of these secrets, and guessing the passphrase from
it is slow. A short or common passphrase can still be guessed, so use a few words.
Not covered: notes and journal entries, the Mesh message history, and Bluetooth pairing
keys, which the Bluetooth stack stores itself.

## Battery saving

**Settings → Dim screen on battery** and **Sleep on battery** set how long the device
waits without a key press (each can be switched off). They only apply on battery, i.e.
when no computer is connected over USB; a charger without data lines counts as battery.

- **Dim:** the built-in screen dims. Pressing any key restores it, and the key works as
  usual.
- **Sleep:** apps save their work, Wi-Fi switches off, both screens go black and the
  processor sleeps. Any key (or the G0 button) wakes it within a moment; that key press
  is not passed on to the app. Wi-Fi reconnects by itself. The device does not sleep
  while an SSH connection is open, because sleeping would drop it.

The external display's backlight is the largest power draw. With its LED pin on G13
(pin 12) the firmware dims it together with the built-in screen and switches it off in
sleep. This needs a display board with a backlight transistor (most 2.8" ILI9341 boards
have one); without one the backlight draws more current than a GPIO should supply. If
your LED pin is instead tied to 5VOUT (pin 6), the backlight is always on: in sleep the
screen is black but still lit. Set `EXT_TFT_BL` in `include/board_config.h` to -1 in
that case, or to another free GPIO if you wired it elsewhere.

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

## Journal

Launcher → **Journal**. Type a line and press Enter: it is added to today's file with
the current time. Each day is one Markdown file, `2026-11-12.md`:

```markdown
# 2026-11-12

- **09:15** First entry of the day
- **14:32** Another entry
```

| Keys | Action |
|------|--------|
| type, Enter | Add an entry to today (also when another day is shown) |
| Left / Right | Previous / next day |
| Up / Down, `fn`+`[` `]` | Scroll |
| `opt` + `d` | Pick a day from a list |
| `opt` + `u` | Remove the last entry of the day shown |
| `opt` + `s` | Sync now |
| `opt` + `a` | Set the sync folder |

Entries need the clock, which is set from the network: after power-on the device has to
reach Wi-Fi once before entries can be added.

**WebDAV sync.** `opt`+`a` asks for a WebDAV folder, user name and password (if Notes
sync is set up, the same server with a `Journal` folder is suggested). The folder is
created if it does not exist. Sync runs when the app is opened, when you leave it with
unsynced entries, and on `opt`+`s`. If a day was changed both on the device and
elsewhere, the entries of both versions are merged in time order, so nothing is lost;
text added by other apps is kept. Other files in the folder are ignored. Certificates
and the stored password work as described for Notes.

## Tasks (Mindwtr)

Launcher → **Tasks**. Shows and edits the tasks of a [Mindwtr](https://mindwtr.app)
library. Mindwtr's WebDAV sync keeps everything in one `data.json` file in a WebDAV
folder. Each time you open the app it connects to a saved Wi-Fi network (if it is not
connected yet) and syncs: it downloads that file and writes your changes back to it, so
they show up in Mindwtr after its next sync. If no saved network is in range, the app
works offline and syncs as soon as Wi-Fi is back.

The app needs a **microSD card**: the downloaded file and your unsynced changes are kept
in `/cardputerOS/tasks/` on the card. Only the sync settings stay on the device (the
password encrypted, like the other secrets). Without a card the app asks you to insert
one; `r` tries again. Do not pull the card while the app shows "Please wait".

1. In Mindwtr, set up **WebDAV sync** and sync once, so the folder has a `data.json`.
2. In Tasks press `opt`+`a` and enter the same folder address (e.g.
   `https://HOST/remote.php/dav/files/USER/Mindwtr/`; the address of the `data.json`
   file itself works too), the user name and password.

| Keys | Action |
|------|--------|
| Tab / Left / Right, `1`–`7` | Switch view: Focus (next actions and anything due), Inbox, Next, Waiting, Someday, All, Done |
| up / down | Move |
| Enter | Open the task: title, status, due and start date, project, contexts, tags, notes, checklist |
| Space or `x` | Mark done / reopen |
| `n` | New task (in the view's status; `@context` and `#tag` words become contexts and tags) |
| `e` | Edit the title |
| `d` | Set the due date |
| `s` | Set the status (inbox, next, waiting, someday, reference, done, archived) |
| `fn`+Backspace (Delete) | Delete (moves it to Mindwtr's trash) |
| `r` or `opt`+`s` | Sync now (connects to Wi-Fi first if needed) |
| `opt`+`a` | Set the WebDAV folder |

In a task, Enter changes the selected field and Esc or Backspace goes back. Dates can be
typed as `today`, `tom`, `fri`, `+3`, `+2w`, `12-24`, `24.12`, `2026-12-24`, each with an
optional time (`fri 14:30`, `tom 2pm`); an empty date removes it. Overdue tasks are red,
tasks due today orange. Notes of more than one line are shown but cannot be edited here.

Changes are saved on the device right away and uploaded a few seconds after the last
one (and when you leave the app). Without Wi-Fi they wait and go up at the next sync;
the header shows how many are waiting. Editing needs the clock to be set (Wi-Fi once
after power-on).

How it merges: the downloaded file is kept on the device as it is, and only the changed
tasks are rewritten, so projects, settings and fields this app does not show are passed
through untouched. Each change bumps the task's revision, as Mindwtr does, so Mindwtr
takes it over. The upload only replaces the file if nobody changed it since it was
downloaded; otherwise the newer file is downloaded and the changes are applied to it
again. Not supported: Mindwtr's end-to-end encrypted sync, and creating the next copy of
a repeating task when it is completed (complete those in Mindwtr). Certificates work as
described for Notes; the password is stored encrypted.

## Calendar

Launcher → **Calendar**. An agenda of calendars you subscribe to by their **.ics
link**: the "Secret address in iCal format" of a Google calendar, a published
Outlook / Office 365 calendar, a public iCloud calendar, or any other `https://` or
`webcal://` link to an `.ics` file. Several calendars can be shown together, each in
its own colour. The calendars are read-only here.

The app needs a **microSD card**: each calendar is saved in `/cardputerOS/calendar/`
as it was downloaded (`<n>.ics`, plus `<n>.meta` with the time it was last checked).
Calendars are downloaded one at a time and written to the card in 4 KB batches. A
calendar is never held in memory as a whole: after each download it is read twice as a
stream and turned into an index on the card (`<n>.occ`: one small record per event in
the next months, `<n>.txt`: the event texts), and the agenda keeps only two weeks of
events in memory, loading the next two weeks from the card as you scroll. Descriptions
are only read when you open an event.
The links usually contain a private token, so they are stored encrypted with the other
secrets.

**Downloading (stale-while-revalidate).** When you open the app it shows the saved copy
straight away, without waiting for the network. Calendars that were last checked more
than 15 minutes ago are then checked in the background while you use the app: the
request carries the saved copy's ETag / Last-Modified, so an unchanged calendar costs
one short request. A new version is written to a temporary file and only replaces the
saved copy once it has arrived completely and looks like a calendar; the agenda then
updates in place. If there is no Wi-Fi or the server fails, the saved copy stays in use
and the header shows how old it is (`offline, as of 14:02`, `1 failed`). A failed check
is retried after 10 minutes, or right away with `r`.

**Wi-Fi.** If a calendar needs checking when you open the app and Wi-Fi is off, the app
joins the strongest saved network by itself (the header shows `connecting to Wi-Fi...`
while the saved agenda stays usable) and **switches Wi-Fi off again once the downloads
are done**. The same goes for `r`. A connection that was already up when you opened the
app is left alone, and so is one in use when you leave the app before the download has
finished (another app may need it then). If no saved network is in range within 30
seconds the app stops trying and shows `no Wi-Fi in range`.

| Keys | Action |
|------|--------|
| up / down, `fn`+`[` `]` | Move through the agenda |
| Left / Right (`,` `/`) | Previous / next day with events |
| `t` | Today (the next event that is not over yet) |
| Enter | Event details: time, place and description (up / down scroll, Esc back) |
| `r` or `opt`+`s` | Update all calendars now (connects to Wi-Fi first if needed) |
| `c` or `opt`+`a` | Calendars: add, update, rename, change the link or colour, remove |

To add one, press `c` → **+ Add calendar**, paste or type the link and give it a name
(leave the name empty to use the calendar's own). If the link needs a user name and
password (e.g. a private feed behind Basic authentication) say so when asked; most
calendar links do not. The first download happens right away.

**Adding calendars from a computer.** Calendar links are long, so instead of typing
them you can put them in a text file on the card, `/cardputerOS/calendar/calendars.txt`,
one calendar per line: an optional name, then the link. Empty lines and lines
starting with `#` are ignored.

```
# name (optional)   link
Work       https://calendar.google.com/calendar/ical/.../basic.ics
Holidays:  webcal://example.com/holidays.ics
https://example.com/team.ics
```

The app reads the file when it is opened, when the card is inserted while the app is
open, or when you choose `c` → **Import from SD card**, which also tells you what it
found if nothing could be imported. The file is also found at the top of the card or in
`/cardputerOS/`, and under names such as `Calendars.txt`, `calendar.txt` or
`calendars.txt.txt` (Windows hides the extension, so a file typed as `calendars.txt`
often ends up with that name). Plain text is expected: UTF-8 or Notepad's "Unicode"
(UTF-16) both work; save from TextEdit with Format → Make Plain Text. It adds the calendars that are not subscribed yet and downloads them right
away (asking about each new server's certificate). Because the links usually contain a
private token, they are then stored encrypted on the device and **the file is deleted
from the card**. Lines that could not be used (no link, or more than 8 calendars) are
written back to the file with a note, so you can fix them. A calendar that needs a user
name and password can be imported this way too; add the login afterwards with `c` →
the calendar → **Change link**.

The agenda covers the last 7 days and the next 120. Repeating events (daily, weekly,
monthly, yearly rules with intervals, counts, end dates, weekdays such as "last Friday",
exceptions and moved or cancelled single dates) and all-day and multi-day events are
shown; times are converted from the calendar's time zone to the one set in Settings.
Events that already ended are grey. Up to 4000 occurrences per calendar are indexed.
Certificates work as described for Notes; a server seen for the first time, or a
changed certificate, is only accepted when you update with `r` (the background check
cannot ask and reports an error instead).

## Mesh (Meshtastic)

Launcher → **Mesh**. A text messenger for [Meshtastic](https://meshtastic.org): the
Cardputer connects over Bluetooth LE to a Meshtastic node (any board running the
Meshtastic firmware), exactly as the phone apps do, and the node does the LoRa part. No
extra pins or LoRa hardware on the Cardputer are needed.

1. Make sure the node has Bluetooth enabled and no phone is connected to it (a node
   accepts one Bluetooth client at a time).
2. In the app press `c` → **Scan for Meshtastic nodes** and pick yours.
3. When asked, enter the pairing PIN shown on the node's screen (or `123456` for nodes
   set to the fixed default PIN). Pairing is remembered; the app reconnects by itself
   afterwards, also after the node was out of range.

| Screen | Keys |
|--------|------|
| Conversations | Enter: open · `n`: node list · `c`: connect / disconnect / scan |
| Chat | type and press Enter to send · arrows or `fn`+`[` `]`: scroll · Esc: back |
| Nodes | Enter: start a direct message · Backspace: back |

Every channel of the node gets a conversation, plus one per node you exchange direct
messages with. After your own messages `~` means sending, `+` delivered (acknowledged)
and `!` failed. The last 60 messages of each conversation are kept across restarts.
While the app is in the background, new messages are announced on the built-in screen.

The app keeps the device awake while it is open (Bluetooth does not survive sleep);
close it from the home screen with `x` to let the device sleep again. Bluetooth, Wi-Fi
and an SSH session together use most of the memory, so close apps you are not using if
something fails to connect.

## Wardrive

Launcher → **Wardrive**. Lists the Wi-Fi networks and Bluetooth LE devices around you
and keeps track of every one seen since the app was opened. Scanning starts right away
and continues while the app is in the background (the built-in screen shows the
counts); it stops when the app is paused or closed.

| Keys | Action |
|------|--------|
| Tab, left / right | Switch between the Wi-Fi and Bluetooth lists |
| up / down | Move the selection (it follows its device when the list re-sorts) |
| Enter | Details: BSSID / address, channel, security, manufacturer, signal, first and last seen |
| `o` | Sort by signal (devices in range first), newest, or name |
| Space | Pause / resume scanning |
| `s` | SD card: show the log file, or try the card again |
| `n` | New session: clear the lists and start a new log file |

Devices seen in the last 30 seconds count as in range and show their signal in dBm;
the others are grey and show how long ago they were last seen. Open Wi-Fi networks are
green, WEP ones orange and hidden ones purple; Bluetooth devices that do not advertise
a name are shown by manufacturer where it is known.

**SD card log.** If a microSD card is inserted when the app starts (or when you press
`s`), each device is written once per session to
`/cardputerOS/wardrive/<date>-<time>.csv` (`session-NNNN.csv` while the clock is not
set). The file is in [WiGLE](https://wigle.net) CSV format, so the usual wardriving
tools can read it; as the Cardputer has no GPS, the location columns are 0, and the
times are UTC and only filled in once the clock is set (Wi-Fi + NTP). New devices are
written every 15 seconds and when you leave the app. Without a card nothing is saved:
the lists simply show what is around. If the card is removed, logging stops (`SD
ERROR`); put it back and press `s`, which writes all devices seen so far again.

While Wardrive is scanning, the automatic Wi-Fi connect is paused (an existing
connection is kept), and the app keeps the device awake. Bluetooth can only be used by
one app at a time: if Mesh is open, Wardrive scans Wi-Fi only and picks up Bluetooth
once Mesh is closed. Mesh, opened while Wardrive is running, asks you to close
Wardrive first.

## Sudoku

Launcher → **Games** → **Sudoku**. Every puzzle is generated on the device and has exactly one
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

Launcher → **Games** → **Ember Crown**. Explore eight randomly generated dungeon levels, fight what
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

## Merge

Launcher → **Games** → **Merge**. A sliding tile puzzle on a 4x4 board: each move
slides every tile as far as it goes in one direction, and two equal tiles that meet
become one tile of twice the value (the merged values are added to the score). After
every move a new 2 (sometimes a 4) appears. Reach a 2048 tile, then keep going if you
like; the game ends when no move is possible.

| Keys | Action |
|------|--------|
| arrows, `;` `.` `,` `/`, `h` `j` `k` `l`, `w` `a` `s` `d` | Slide up / down / left / right |
| `u` or Backspace | Undo the last move |
| `n` | New game |

The game in progress and the best score are kept across restarts.

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
lib/journal              journal file format and merging (portable, unit tested)
lib/mindwtr              streaming JSON and Mindwtr task edits for Tasks (portable, unit tested)
lib/mesh                 Meshtastic client protocol and message store (portable, unit tested)
lib/rogue                roguelike game rules (portable, unit tested with a bot player)
lib/merge                sliding tile puzzle rules (portable, unit tested)
lib/wardrive             wardriving device table and WiGLE CSV log (portable, unit tested)
lib/vault                encryption of stored secrets (portable, unit tested)
src/apps                 apps: settings, ssh, notes, journal, tasks, mesh, wardrive, sudoku, rogue, merge
test/                    host-side unit tests
```

## Adding an app

Create a `.cpp` under `src/apps`, derive from `os::App` (see `src/os/app.h`) and register it:

```cpp
class NotesApp : public os::App {
    void onResume() override { /* paint into os::content() using os::gfx() */ }
    void onKey(const key::Event& ev) override { /* ... */ }
};
// name, description, colour, icon (src/os/icons.h, or nullptr for a letter tile), position
OS_REGISTER_APP(NotesApp, "Notes", "Markdown notes", theme::dracula::foreground, icons::note, 20);
```

It appears on the home screen automatically, ordered by the last number. To put it in a
home screen folder, use `OS_REGISTER_APP_IN(Class, "Games", name, ...)` with the same
arguments after the folder name; the folder is created as soon as one app uses it. `ui::List`,
`ui::prompt`, `ui::confirm` and `os::TermView` are available as building blocks.
