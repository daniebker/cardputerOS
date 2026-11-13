-- Hello: a small tour of what a Lua app can do. Copy this folder to
-- /cardputerOS/apps/hello on the SD card, then Settings -> SD card apps.

local app = {}

local presses = 0      -- kept across restarts with store.get / store.set
local last = "none yet"
local top = 0          -- y below the header line

-- Called once when the app is opened.
function app.start()
  presses = store.get("presses", 0)
end

-- Repaints only the lines that change, so the screen does not flicker.
local function drawStatus()
  gfx.text("Last key: " .. last .. "      ", 10, top + 40, colors.cyan, {bg = colors.bg})
  gfx.text("Keys pressed so far: " .. presses, 10, top + 62, colors.dim, {font = "small", bg = colors.bg})
end

-- Called whenever the whole screen must be painted: when the app opens, when
-- you come back from the home screen, and after a dialog closes.
function app.draw()
  gfx.clear()
  top = ui.header("Hello from Lua")
  local name = store.get("name")
  gfx.text(name and ("Hello, " .. name .. "!") or "Press any key.", 10, top + 12, colors.text, {font = "bold"})
  drawStatus()
  gfx.fill_round_rect(10, 150, 300, 40, 8, colors.panel)
  gfx.text("cardputerOS " .. sys.version, 160, 163, colors.purple, {align = "center"})
  ui.footer("n: your name   q: quit   opt+`: home screen")
end

-- Called for every key press. k.key is the key's name ("a", "enter", "up", ...).
function app.key(k)
  if k.key == "q" then
    sys.exit()
  elseif k.key == "n" then
    local name = ui.prompt("Your name", store.get("name", ""))
    if name then store.set("name", name) end
    -- no need to repaint here: draw() runs after a dialog
  else
    presses = presses + 1
    store.set("presses", presses)
    last = k.key
    drawStatus()
  end
end

return app
