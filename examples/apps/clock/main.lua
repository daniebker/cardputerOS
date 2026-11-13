-- Clock: the time, the date, and a countdown timer that keeps running while
-- the app is in the background (the built-in screen tells you when it is done).

local app = {}

local timerEnd = nil   -- sys.millis() when the timer is done
local doneUnseen = false
local shown = ""       -- what is on the screen, to repaint only on changes

local function mmss(ms)
  local s = (math.max(0, ms) + 999) // 1000
  return string.format("%d:%02d", s // 60, s % 60)
end

local function drawClock()
  local t = sys.time()  -- nil until the clock was set over Wi-Fi
  local time = t and os.date("%H:%M:%S", t) or "--:--:--"
  local date = t and os.date("%A, %d %B %Y", t) or "Connect to Wi-Fi to set the clock"
  local info = timerEnd and ("Timer " .. mmss(timerEnd - sys.millis())) or ""
  local key = time .. date .. info
  if key == shown then return end
  shown = key
  gfx.fill_rect(0, 50, 320, 150, colors.bg)
  gfx.text(time, 160, 60, colors.text, {font = "mono", size = 5, align = "center"})
  gfx.text(date, 160, 115, colors.dim, {align = "center"})
  gfx.text(info, 160, 150, colors.orange, {font = "bold", size = 2, align = "center"})
  local battery, wifi = sys.battery(), sys.wifi()
  gfx.text("Battery " .. (battery and battery .. "%" or "?") .. "     Wi-Fi " .. (wifi or "off"),
           160, 200, colors.dim, {font = "small", align = "center"})
end

local function checkTimer()
  if timerEnd and sys.millis() >= timerEnd then
    timerEnd = nil
    doneUnseen = true
    sys.notice("Clock: the timer is done")  -- shown even while in the background
    sys.status(nil)
  end
end

function app.start()
  sys.interval(100)  -- update() 10 times a second is plenty
end

function app.draw()
  if doneUnseen then
    doneUnseen = false
    sys.notice(nil)
    ui.alert("Timer", "Time is up!")  -- draws over the screen; we paint everything next
  end
  gfx.clear()
  ui.header("Clock")
  ui.footer("t: timer   c: cancel timer   q: quit")
  shown = ""
  drawClock()
end

function app.update(dt)
  checkTimer()
  if doneUnseen then app.draw() else drawClock() end
end

function app.background(dt)
  checkTimer()
end

local CHOICES = {1, 3, 5, 10, 25}

function app.key(k)
  if k.key == "q" then
    sys.exit()
  elseif k.key == "c" then
    timerEnd = nil
    sys.status(nil)
    drawClock()
  elseif k.key == "t" then
    local items = {}
    for i, m in ipairs(CHOICES) do items[i] = m .. (m == 1 and " minute" or " minutes") end
    items[#items + 1] = "Other..."
    local pick = ui.choose("Timer", items)
    local minutes = CHOICES[pick or 0]
    if pick == #items then minutes = tonumber(ui.prompt("Minutes", "") or "") end
    if minutes and minutes > 0 then
      timerEnd = sys.millis() + math.floor(minutes * 60000)
      sys.status("Timer " .. minutes .. " min")
    end
  end
end

return app
