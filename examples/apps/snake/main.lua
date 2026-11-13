-- Snake: eat the apples, grow, and do not run into the walls or yourself.
-- Shows a game loop with app.update(dt), incremental drawing and a best score.

local app = {}

local CELL = 12
local COLS, ROWS = 26, 18
local OX, OY = (320 - COLS * CELL) // 2, 240 - ROWS * CELL - 2  -- board position
local BODY = gfx.rgb(40, 170, 80)

local DIRS = {up = {0, -1}, down = {0, 1}, left = {-1, 0}, right = {1, 0}}
local OPPOSITE = {up = "down", down = "up", left = "right", right = "left"}
-- arrows (fn + ; . , /), the same keys without fn, vi keys and WASD
local KEYS = {
  up = "up", [";"] = "up", k = "up", w = "up",
  down = "down", ["."] = "down", j = "down", s = "down",
  left = "left", [","] = "left", h = "left", a = "left",
  right = "right", ["/"] = "right", l = "right", d = "right",
}

local snake, dir, nextDir, apple, score, best, over, paused, wait, speed

local function cell(x, y, color)
  gfx.fill_rect(OX + x * CELL + 1, OY + y * CELL + 1, CELL - 2, CELL - 2, color)
end

local function occupied(x, y)
  for _, p in ipairs(snake) do
    if p[1] == x and p[2] == y then return true end
  end
  return false
end

local function placeApple()
  repeat
    apple = {math.random(0, COLS - 1), math.random(0, ROWS - 1)}
  until not occupied(apple[1], apple[2])
end

local function reset()
  snake = {{5, 9}, {4, 9}, {3, 9}}  -- head first
  dir, nextDir = "right", "right"
  score, over, paused, wait, speed = 0, false, false, 0, 140
  placeApple()
end

local function drawScore()
  gfx.fill_rect(0, 0, 320, OY - 1, colors.bg)
  gfx.text("Score " .. score, OX, 3, colors.text, {font = "bold"})
  gfx.text("Best " .. best, OX + COLS * CELL, 5, colors.yellow, {align = "right"})
  sys.status("Score " .. score .. "   best " .. best)  -- on the built-in screen
end

local function banner(text)
  local w = gfx.text_width(text, "bold") + 30
  gfx.fill_round_rect(160 - w // 2, 112, w, 30, 8, colors.panel)
  gfx.round_rect(160 - w // 2, 112, w, 30, 8, colors.purple)
  gfx.text(text, 160, 119, colors.text, {font = "bold", align = "center"})
end

function app.start()
  best = store.get("best", 0)
  reset()
end

function app.draw()
  gfx.clear()
  gfx.rect(OX - 1, OY - 1, COLS * CELL + 2, ROWS * CELL + 2, colors.current_line)
  for i, p in ipairs(snake) do cell(p[1], p[2], i == 1 and colors.green or BODY) end
  cell(apple[1], apple[2], colors.red)
  drawScore()
  if over then banner("Game over - Enter: play again")
  elseif paused then banner("Paused - space: go on") end
end

local function gameOver()
  over = true
  if score > best then
    best = score
    store.set("best", best)
  end
  app.draw()
end

local function step()
  dir = nextDir
  local d = DIRS[dir]
  local head = snake[1]
  local x, y = head[1] + d[1], head[2] + d[2]
  if x < 0 or y < 0 or x >= COLS or y >= ROWS then return gameOver() end
  local eating = x == apple[1] and y == apple[2]
  if not eating then  -- the tail moves on, so the head may take its place
    local tail = table.remove(snake)
    cell(tail[1], tail[2], colors.bg)
  end
  if occupied(x, y) then return gameOver() end
  cell(head[1], head[2], BODY)
  table.insert(snake, 1, {x, y})
  cell(x, y, colors.green)
  if eating then
    score = score + 1
    speed = math.max(60, speed - 3)
    if #snake >= COLS * ROWS then return gameOver() end
    placeApple()
    cell(apple[1], apple[2], colors.red)
    drawScore()
  end
end

-- Runs about 30 times a second; dt is the time since the last call in ms.
function app.update(dt)
  if over or paused then return end
  wait = wait + dt
  if wait >= speed then
    wait = 0
    step()
  end
end

function app.key(k)
  if k.ctrl or k.alt or k.opt then return end
  if over then
    if k.key == "enter" or k.key == "space" then
      reset()
      app.draw()
    end
  elseif k.key == "space" or k.key == "p" then
    paused = not paused
    app.draw()
  elseif k.key == "n" then
    if ui.confirm("New game", "Start over?") then reset() end
  else
    local want = KEYS[k.key]
    if want and want ~= OPPOSITE[dir] then nextDir = want end
  end
end

-- Leaving for the home screen pauses the game.
function app.pause()
  if not over then paused = true end
end

return app
