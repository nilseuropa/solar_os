local solaros = require("solaros")
local gfx = solaros.gfx
local audio = solaros.audio

local CELL = 10
local STEP_MS = 100
local POLL_MS = 20
local START_LEN = 5

local LEFT = {-1, 0}
local RIGHT = {1, 0}
local UP = {0, -1}
local DOWN = {0, 1}

local KEY_SPACE = 32
local KEY_ENTER = 13
local KEY_LF = 10
local KEY_A = 97
local KEY_D = 100
local KEY_H = 104
local KEY_J = 106
local KEY_K = 107
local KEY_L = 108
local KEY_P = 112
local KEY_Q = 113
local KEY_R = 114
local KEY_S = 115
local KEY_W = 119

local function tone(freq, ms, volume)
    local ok = pcall(audio.tone, freq, ms, volume or 35)
    return ok
end

local function rand_next(seed)
    return (seed * 109 + 1021) % 32768
end

local function new_food(seed, cols, rows, snake)
    for _ = 1, cols * rows do
        seed = rand_next(seed)
        local x = seed % cols
        seed = rand_next(seed)
        local y = seed % rows
        local found = false
        for i = 1, #snake do
            if snake[i][1] == x and snake[i][2] == y then
                found = true
            end
        end
        if not found then
            return seed, {x, y}
        end
    end

    return seed, {0, 0}
end

local function opposite(a, b)
    return a[1] + b[1] == 0 and a[2] + b[2] == 0
end

local function apply_key(key, direction, paused)
    if key == gfx.KEY_ESCAPE or key == KEY_Q then
        return direction, paused, true
    end
    if key == KEY_SPACE or key == KEY_P then
        paused = not paused
    end

    local next_dir = direction
    if key == gfx.KEY_LEFT or key == KEY_A or key == KEY_H then
        next_dir = LEFT
    elseif key == gfx.KEY_RIGHT or key == KEY_D or key == KEY_L then
        next_dir = RIGHT
    elseif key == gfx.KEY_UP or key == KEY_W or key == KEY_K then
        next_dir = UP
    elseif key == gfx.KEY_DOWN or key == KEY_S or key == KEY_J then
        next_dir = DOWN
    end

    if not opposite(direction, next_dir) then
        direction = next_dir
    end
    return direction, paused, false
end

local function read_key(direction, paused, timeout_ms)
    local remaining = timeout_ms
    while remaining > 0 do
        local chunk = math.min(remaining, POLL_MS)
        local key = gfx.getch(chunk)
        while key ~= nil do
            local quit_requested
            direction, paused, quit_requested = apply_key(key, direction, paused)
            if quit_requested then
                return direction, paused, true
            end
            key = gfx.getch(0)
        end
        remaining = remaining - chunk
    end
    return direction, paused, false
end

local function draw(w, h, board_x, board_y, cols, rows, snake, food, score, paused, game_over)
    gfx.clear(gfx.WHITE)

    gfx.color(gfx.BLACK)
    gfx.font(gfx.FONT_BOLD_14)
    gfx.text(board_x, 16, "Snake")
    gfx.font(gfx.FONT_MONO_12)
    gfx.text(board_x + 70, 16, "score " .. score)

    gfx.color(gfx.LIGHT)
    for x = 0, cols do
        local px = board_x + x * CELL
        gfx.line(px, board_y, px, board_y + rows * CELL)
    end
    for y = 0, rows do
        local py = board_y + y * CELL
        gfx.line(board_x, py, board_x + cols * CELL, py)
    end

    gfx.color(gfx.BLACK)
    gfx.rect(board_x - 1, board_y - 1, cols * CELL + 2, rows * CELL + 2)

    gfx.color(gfx.DARK)
    gfx.fill_rect(board_x + food[1] * CELL + 2, board_y + food[2] * CELL + 2, CELL - 4, CELL - 4)

    for i = 1, #snake do
        local part = snake[i]
        gfx.color(i == 1 and gfx.BLACK or gfx.DARK)
        gfx.fill_rect(board_x + part[1] * CELL + 1, board_y + part[2] * CELL + 1, CELL - 2, CELL - 2)
        if i == 1 then
            gfx.color(gfx.WHITE)
            gfx.pixel(board_x + part[1] * CELL + 3, board_y + part[2] * CELL + 3)
        end
    end

    gfx.color(gfx.BLACK)
    gfx.font(gfx.FONT_MONO_12)
    if game_over then
        local msg = "Game over - Enter restarts, ESC quits"
        gfx.text(math.max(0, (w - #msg * 7) // 2), h - 10, msg)
    elseif paused then
        local msg = "Paused - Space resumes"
        gfx.text(math.max(0, (w - #msg * 7) // 2), h - 10, msg)
    else
        gfx.text(board_x, h - 10, "Arrows/WASD move, Space pauses, ESC quits")
    end

    gfx.refresh()
end

local function contains(snake, point, count)
    for i = 1, count do
        if snake[i][1] == point[1] and snake[i][2] == point[2] then
            return true
        end
    end
    return false
end

local function reset(cols, rows)
    local cx = cols // 2
    local cy = rows // 2
    local snake = {}
    for i = 0, START_LEN - 1 do
        snake[#snake + 1] = {cx - i, cy}
    end
    return snake, RIGHT, false, false, 0
end

gfx.begin()

local ok, err = pcall(function()
    local w, h = gfx.size()
    local cols = math.max(12, math.min((w - 20) // CELL, 38))
    local rows = math.max(10, math.min((h - 44) // CELL, 25))
    local board_x = (w - cols * CELL) // 2
    local board_y = 24 + math.max(0, (h - 44 - rows * CELL) // 2)

    local seed = (w + h + cols + rows) % 32768 + 1
    local snake, direction, paused, game_over, score = reset(cols, rows)
    local food
    seed, food = new_food(seed, cols, rows, snake)

    draw(w, h, board_x, board_y, cols, rows, snake, food, score, paused, game_over)

    while not solaros.should_exit() do
        local quit_requested
        direction, paused, quit_requested = read_key(direction, paused, STEP_MS)
        if quit_requested then
            break
        end

        if game_over then
            local key = gfx.getch(80)
            if key == gfx.KEY_ESCAPE or key == KEY_Q then
                break
            end
            if key == KEY_ENTER or key == KEY_LF or key == KEY_R or key == KEY_SPACE then
                snake, direction, paused, game_over, score = reset(cols, rows)
                seed, food = new_food(seed, cols, rows, snake)
                tone(880, 40)
                draw(w, h, board_x, board_y, cols, rows, snake, food, score, paused, game_over)
            end
        else
            if paused then
                draw(w, h, board_x, board_y, cols, rows, snake, food, score, paused, game_over)
            else
                local head = snake[1]
                local next_head = {head[1] + direction[1], head[2] + direction[2]}

                local body_count = #snake
                if not (next_head[1] == food[1] and next_head[2] == food[2]) then
                    body_count = body_count - 1
                end

                if next_head[1] < 0 or next_head[1] >= cols or
                    next_head[2] < 0 or next_head[2] >= rows or
                    contains(snake, next_head, body_count) then
                    game_over = true
                    tone(180, 120, 45)
                else
                    table.insert(snake, 1, next_head)
                    if next_head[1] == food[1] and next_head[2] == food[2] then
                        score = score + 1
                        tone(740 + math.min(score, 12) * 20, 35)
                        seed, food = new_food(seed, cols, rows, snake)
                    else
                        table.remove(snake)
                    end
                end

                draw(w, h, board_x, board_y, cols, rows, snake, food, score, paused, game_over)
            end
        end
    end
end)

gfx["end"]()

if not ok then
    error(err)
end
