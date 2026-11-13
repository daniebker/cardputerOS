# Writing Lua apps

cardputerOS runs apps written in [Lua 5.4](https://www.lua.org/manual/5.4/) from the
microSD card. You don't need to build or flash any firmware: put a folder on the card
and the app shows up on the home screen next to the built-in ones.

## Quick start

1. On the SD card, create the folder `/cardputerOS/apps/hello/`.
2. Put a file `main.lua` in it:

   ```lua
   local app = {}

   function app.draw()
     gfx.clear()
     gfx.text("Hello, world!", 160, 110, colors.green, {font = "large", align = "center"})
   end

   function app.key(k)
     if k.key == "q" then sys.exit() end
   end

   return app
   ```

3. Put the card in the Cardputer and choose **Settings → SD card apps** (or restart).
   The app is now on the home screen.

The `examples/apps` folder of this repository has three complete apps (`hello`, `snake`,
`clock`): copy them to `/cardputerOS/apps/` on the card to try them, or as a starting point.

## The app folder

```
/cardputerOS/apps/<id>/
    main.lua     required: the app
    app.ini      optional: name, icon, colour, ...
    ...          anything else the app reads (modules, levels, images as text)
```

`<id>` is the folder name: letters, digits, `-`, `_` and `.` only, at most 31
characters. It identifies the app, for example for its save data.

`app.ini` (all lines optional):

```ini
name = Snake                      ; shown under the icon (default: the folder name)
description = Eat and grow        ; shown at the bottom of the home screen
color = green                     ; icon colour: #RRGGBB or red, orange, yellow, green,
                                  ; cyan, purple, pink, white, grey
icon = tiles                      ; a built-in icon: terminal, note, gear, sudoku, crown,
                                  ; mesh, journal, tiles, radar (default: first letter)
folder = Games                    ; put it in a home screen folder
```

The card is scanned at startup and when you choose **Settings → SD card apps**,
which also lists folders that could not be used and why. The code is read from the
card each time the app starts, so to try a change, close the app (`x` on the home
screen) and open it again. Running apps keep running while you rescan.

## How an app works

`main.lua` runs once when the app starts and must end with `return app`, where
`app` is a table of functions. The OS calls them when something happens; all of
them are optional:

| Function | Called |
|----------|--------|
| `app.start()` | once, when the app is opened (after the save data is loaded) |
| `app.draw()` | when the whole screen must be painted: on opening, on coming back from the home screen, and after a dialog closes |
| `app.key(k)` | for each key press (see [Keys](#keys)) |
| `app.update(dt)` | about 30 times a second while the app is shown; `dt` is the milliseconds since the last call. `sys.interval(ms)` changes the rate |
| `app.background(dt)` | the same, while the app runs in the background (it can't draw then) |
| `app.pause()` | when the user goes to the home screen, or the device goes to sleep |
| `app.stop()` | when the app is closed |

The screen is 320×240 pixels and belongs to the app. Nothing is repainted for
you: draw what changes, when it changes. `opt`+`` ` `` (or the G0 button) always
goes back to the home screen. The app then keeps running in the background until
it is closed with `x` on the home screen or calls `sys.exit()`.

A callback must return within 2 seconds (time the user spends in a dialog does
not count), or the app is stopped. This prevents an endless loop from freezing
the device. So do long work in steps, for example a little in each `app.update`.

### Errors

If a function raises an error, the app stops and the screen shows the message and
where it happened (`main.lua:12: attempt to index a nil value`). Press `r` to
read the app from the card again and restart it, or Enter to close it. `print()`
output and errors also go to the USB serial console (`pio device monitor`, 115200
baud), prefixed with the app's id.

## Keys

`app.key(k)` gets a table:

| Field | |
|-------|-|
| `k.key` | the key: the character itself (`"a"`, `"A"`, `"7"`, `"?"`), or `"enter"`, `"esc"`, `"backspace"`, `"tab"`, `"space"`, `"up"`, `"down"`, `"left"`, `"right"`, `"home"`, `"end"`, `"pageup"`, `"pagedown"`, `"delete"`, `"f1"` ... `"f10"` |
| `k.char` | the character typed, or nil for keys that don't type one (and with ctrl / alt / opt) |
| `k.ctrl`, `k.alt`, `k.shift`, `k.opt` | modifier keys held (true / false) |
| `k.repeated` | true if the key is held down and this is a repeat |

The Cardputer has no separate arrow keys: `fn` + `;` `.` `,` `/` send `"up"`,
`"down"`, `"left"`, `"right"`, and `fn` + `` ` `` sends `"esc"`. Games often accept
`;` `.` `,` `/` (without `fn`) as arrows too; see the snake example.

## Drawing: `gfx`

Colours are numbers `0xRRGGBB`, a name from `colors`, or a string such as
`"#FF8800"` or `"red"`. Coordinates may be fractional (they are rounded down).
Drawing calls do nothing while the app is in the background.

| Function | |
|----------|-|
| `gfx.width()`, `gfx.height()` | 320, 240 |
| `gfx.clear([color])` | fill the screen (default `colors.bg`) |
| `gfx.fill_rect(x, y, w, h, color)` / `gfx.rect(...)` | filled / outlined rectangle |
| `gfx.fill_round_rect(x, y, w, h, r, color)` / `gfx.round_rect(...)` | with rounded corners of radius r |
| `gfx.line(x1, y1, x2, y2, color)` | |
| `gfx.pixel(x, y, color)` | |
| `gfx.fill_circle(x, y, r, color)` / `gfx.circle(...)` | centre and radius |
| `gfx.fill_triangle(x1, y1, x2, y2, x3, y3, color)` / `gfx.triangle(...)` | |
| `gfx.text(text, x, y [, color [, options]])` | draws text with its top at y; returns its width |
| `gfx.text_width(text [, font [, size]])` | width in pixels |
| `gfx.font_height([font [, size]])` | line height in pixels |
| `gfx.rgb(r, g, b)` | a colour from 0-255 parts |

`gfx.text` options (a table, all optional):

- `font`: `"small"` (9 px), `"normal"` (12 px, the default), `"large"` (18 px),
  `"bold"` (bold sans), or `"mono"` (the 6×8 terminal font)
- `size`: 1 to 8, enlarges the font that many times (`{font = "mono", size = 4}`
  for a big clock)
- `align`: `"left"` (default), `"center"` or `"right"`: x is then the left edge,
  centre or right edge of the text
- `bg`: a colour to fill behind the text first, to overwrite what was there
  without clearing it separately

The fonts show ASCII; accented letters are drawn without their accents.

`colors` has the colour names of the built-in apps (`bg`, `panel`, `text`, `dim`,
`accent`, `select`, `ok`, `warn`, `error`) and the Dracula palette (`background`,
`current_line`, `foreground`, `comment`, `cyan`, `green`, `orange`, `pink`,
`purple`, `red`, `yellow`, `darker`, `darkest`), plus `black` and `white`.

## Dialogs and UI: `ui`

These look like the built-in apps. Dialogs block until answered and draw over the
screen; afterwards `app.draw()` is called to repaint (dialogs opened from inside
`app.draw` are the exception: draw after them yourself). In the background,
dialogs are not shown and return at once as if cancelled.

| Function | Returns |
|----------|---------|
| `ui.alert(title, message)` | |
| `ui.confirm(title, message [, default_yes])` | true / false (y / n; Enter gives the default) |
| `ui.prompt(title [, text [, secret]])` | the text entered, or nil if cancelled |
| `ui.choose(title, items [, selected])` | the number of the item picked (1-based), or nil |
| `ui.busy(message)` | shows a "please wait" box and returns at once |
| `ui.header(title)` | draws a title line at the top; returns the y below it |
| `ui.footer(hint)` | draws a key hint line at the bottom; returns its top y |

## Saving data: `store`

A small key/value store for settings, scores and game state. It's kept in the
Cardputer's internal memory (not on the card), separately for each app, and
survives restarts and SD card changes.

| Function | |
|----------|-|
| `store.get(key [, default])` | the stored value, or `default` |
| `store.set(key, value)` | value: a string, number, boolean, or a table of those (nested tables too); nil deletes |
| `store.save()` | write now (normally not needed) |

Changes are written when the app goes to the background, is closed or stops with
an error, and every 10 seconds otherwise, so calling `store.set` often is fine. If
you change a table after storing it, call `store.set` with it again so the change
is noticed and saved. All the data of an app together may be up to 64 KB.

## Files: `fs`

Files in the app's own folder on the SD card. Paths are relative to it
(`"levels/1.txt"`); an app can't reach files outside its folder.

| Function | Returns |
|----------|---------|
| `fs.read(path)` | the contents (at most 64 KB), or nil and an error message |
| `fs.write(path, data)`, `fs.append(path, data)` | true, or nil and a message; folders are created |
| `fs.list([folder])` | a list of `{name =, dir =, size =}`, or nil and a message |
| `fs.exists(path)` | true / false |
| `fs.remove(path)` | true, or nil and a message |

Each call reads the card directly (the card is only mounted for that moment, so it
can be taken out at any time). That takes a few milliseconds, so read files once,
for example in `app.start`, rather than in every update.

`require("name")` loads `name.lua` from the app folder (`require("lib.util")`
loads `lib/util.lua`), once, like in standard Lua.

## System: `sys`

| | |
|-|-|
| `sys.millis()` | milliseconds since the device started |
| `sys.time()` | the time as a Unix timestamp, or nil while the clock is not set (it is set over Wi-Fi). `os.date("%H:%M", sys.time())` formats it in the local time zone |
| `sys.battery()` | battery level in percent, or nil |
| `sys.wifi()` | name of the Wi-Fi network connected to, or nil |
| `sys.exit()` | close the app (after the current function returns) |
| `sys.status(text)` | short text on the built-in screen next to the app name (nil clears it) |
| `sys.notice(text)` | a line on the built-in screen that is shown even while the app is in the background, e.g. "Timer done" (nil clears it) |
| `sys.keep_awake(on)` | true: the device doesn't go to sleep on battery while the app is open |
| `sys.interval([ms])` | how often `app.update` / `app.background` run (10-60000 ms; 0 = never); returns the current value |
| `sys.memory()` | Lua memory used and the limit, in bytes |
| `sys.version`, `sys.app`, `sys.name` | the OS version, this app's id and name |

## Standard Lua

Available: the basic functions (`print`, `pairs`, `pcall`, `tostring`, `load` for
source text, ...), `string`, `table`, `math`, `utf8`, `coroutine`, and from `os` only
`time`, `clock`, `date` and `difftime`. Not available: `io`, `debug`, `package`,
`dofile` / `loadfile`, and loading precompiled bytecode. Use `fs` and `require`
instead.

## Limits

- **Memory.** An app gets the free memory minus a reserve for the system, at most
  160 KB (typically 100-160 KB; less while Wi-Fi, SSH or Bluetooth apps are open).
  Running out raises a Lua error ("not enough memory"), which `pcall` can catch.
  `sys.memory()` shows the use.
- **Time.** Each callback may run for at most 2 seconds (`app.start` and loading
  `main.lua`: 5 seconds).
- **Nesting.** C functions calling back into Lua (a `pcall` inside a `pcall`, a
  metamethod, `require` inside a module) may nest 40 levels deep. Ordinary Lua
  function calls and recursion are not limited by this, only by memory.
- **Sizes.** `main.lua` up to 128 KB; modules and files read with `fs.read` up to
  64 KB each.

The limits keep a buggy app from taking the device down. They don't make it safe
to run code you don't trust: an app can still, for example, fill its folder on the
card or keep the device awake. Install apps the way you would install any other
program.
