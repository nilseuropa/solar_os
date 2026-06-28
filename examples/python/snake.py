import solaros


gfx = solaros.gfx
audio = solaros.audio

CELL = 10
STEP_MS = 100
POLL_MS = 20
START_LEN = 5

LEFT = (-1, 0)
RIGHT = (1, 0)
UP = (0, -1)
DOWN = (0, 1)

KEY_SPACE = 32
KEY_ENTER = 13
KEY_LF = 10
KEY_A = 97
KEY_D = 100
KEY_H = 104
KEY_J = 106
KEY_K = 107
KEY_L = 108
KEY_P = 112
KEY_Q = 113
KEY_R = 114
KEY_S = 115
KEY_W = 119


def idiv(a, b):
    q = 0
    while a >= b:
        a = a - b
        q = q + 1
    return q


def clamp(value, low, high):
    if value < low:
        return low
    if value > high:
        return high
    return value


def cap(value, high):
    if value > high:
        return high
    return value


def positive(value):
    if value < 0:
        return 0
    return value


def tone(freq, ms, volume=35):
    audio.tone(freq, ms, volume)


def rand_next(seed):
    return (seed * 109 + 1021) % 32768


def new_food(seed, cols, rows, snake):
    for _ in range(cols * rows):
        seed = rand_next(seed)
        x = seed % cols
        seed = rand_next(seed)
        y = seed % rows
        found = False
        for part in snake:
            if part[0] == x and part[1] == y:
                found = True
        if not found:
            return seed, (x, y)

    return seed, (0, 0)


def opposite(a, b):
    return a[0] + b[0] == 0 and a[1] + b[1] == 0


def apply_key(key, direction, paused):
    if key == gfx.KEY_ESCAPE or key == KEY_Q:
        return direction, paused, True
    if key == KEY_SPACE or key == KEY_P:
        paused = not paused
    next_dir = direction
    if key == gfx.KEY_LEFT or key == KEY_A or key == KEY_H:
        next_dir = LEFT
    elif key == gfx.KEY_RIGHT or key == KEY_D or key == KEY_L:
        next_dir = RIGHT
    elif key == gfx.KEY_UP or key == KEY_W or key == KEY_K:
        next_dir = UP
    elif key == gfx.KEY_DOWN or key == KEY_S or key == KEY_J:
        next_dir = DOWN
    if not opposite(direction, next_dir):
        direction = next_dir
    return direction, paused, False


def read_key(direction, paused, timeout_ms):
    remaining = timeout_ms
    while remaining > 0:
        chunk = cap(remaining, POLL_MS)
        key = gfx.getch(chunk)
        while key != None:
            if key == gfx.KEY_ESCAPE or key == KEY_Q:
                return direction, paused, True
            direction, paused, quit_requested = apply_key(key, direction, paused)
            if quit_requested:
                return direction, paused, True
            key = gfx.getch(0)
        remaining = remaining - chunk
    return direction, paused, False


def draw(w, h, board_x, board_y, cols, rows, snake, food, score, paused, game_over):
    gfx.clear(gfx.WHITE)

    gfx.color(gfx.BLACK)
    gfx.font(gfx.FONT_BOLD_14)
    gfx.text(board_x, 16, "Snake")
    gfx.font(gfx.FONT_MONO_12)
    gfx.text(board_x + 70, 16, "score")

    gfx.color(gfx.LIGHT)
    for x in range(cols + 1):
        px = board_x + x * CELL
        gfx.line(px, board_y, px, board_y + rows * CELL)
    for y in range(rows + 1):
        py = board_y + y * CELL
        gfx.line(board_x, py, board_x + cols * CELL, py)

    gfx.color(gfx.BLACK)
    gfx.rect(board_x - 1, board_y - 1, cols * CELL + 2, rows * CELL + 2)

    fx = food[0]
    fy = food[1]
    gfx.color(gfx.DARK)
    gfx.fill_rect(board_x + fx * CELL + 2, board_y + fy * CELL + 2, CELL - 4, CELL - 4)

    for i in range(len(snake)):
        x = snake[i][0]
        y = snake[i][1]
        if i == 0:
            gfx.color(gfx.BLACK)
        else:
            gfx.color(gfx.DARK)
        gfx.fill_rect(board_x + x * CELL + 1, board_y + y * CELL + 1, CELL - 2, CELL - 2)
        if i == 0:
            gfx.color(gfx.WHITE)
            gfx.pixel(board_x + x * CELL + 3, board_y + y * CELL + 3)

    gfx.color(gfx.BLACK)
    gfx.font(gfx.FONT_MONO_12)
    if game_over:
        msg = "Game over - Enter restarts, ESC quits"
        gfx.text(positive(idiv(w - len(msg) * 7, 2)), h - 10, msg)
    elif paused:
        msg = "Paused - Space resumes"
        gfx.text(positive(idiv(w - len(msg) * 7, 2)), h - 10, msg)
    else:
        gfx.text(board_x, h - 10, "Arrows/WASD move, Space pauses, ESC quits")

    gfx.refresh()


def main():
    gfx.begin()
    w, h = gfx.size()
    cols = clamp(idiv(w - 20, CELL), 12, 38)
    rows = clamp(idiv(h - 44, CELL), 10, 25)
    board_x = idiv(w - cols * CELL, 2)
    board_y = 24 + positive(idiv(h - 44 - rows * CELL, 2))

    seed = (w + h + cols + rows) % 32768
    seed = seed + 1

    cx = idiv(cols, 2)
    cy = idiv(rows, 2)
    snake = []
    for i in range(START_LEN):
        snake.append((cx - i, cy))
    direction = RIGHT
    paused = False
    game_over = False
    score = 0

    seed, food = new_food(seed, cols, rows, snake)
    draw(w, h, board_x, board_y, cols, rows, snake, food, score, paused, game_over)

    while not solaros.should_exit():
        direction, paused, quit_requested = read_key(direction, paused, STEP_MS)
        if quit_requested:
            break

        if game_over:
            key = gfx.getch(80)
            if key == gfx.KEY_ESCAPE or key == KEY_Q:
                break
            if key == KEY_ENTER or key == KEY_LF or key == KEY_R or key == KEY_SPACE:
                cx = idiv(cols, 2)
                cy = idiv(rows, 2)
                snake = []
                for i in range(START_LEN):
                    snake.append((cx - i, cy))
                direction = RIGHT
                paused = False
                game_over = False
                score = 0
                seed, food = new_food(seed, cols, rows, snake)
                tone(880, 40)
                draw(w, h, board_x, board_y, cols, rows, snake, food, score, paused, game_over)
            continue

        if paused:
            draw(w, h, board_x, board_y, cols, rows, snake, food, score, paused, game_over)
            continue

        hx = snake[0][0]
        hy = snake[0][1]
        nx = hx + direction[0]
        ny = hy + direction[1]
        next_head = (nx, ny)

        body_count = len(snake)
        if next_head != food:
            body_count = body_count - 1
        collision = False
        for i in range(body_count):
            if snake[i] == next_head:
                collision = True

        if nx < 0 or nx >= cols or ny < 0 or ny >= rows or collision:
            game_over = True
            tone(180, 120, 45)
            draw(w, h, board_x, board_y, cols, rows, snake, food, score, paused, game_over)
            continue

        snake.insert(0, next_head)
        if next_head == food:
            score = score + 1
            tone(740 + cap(score, 12) * 20, 35)
            seed, food = new_food(seed, cols, rows, snake)
        else:
            snake.pop()

        draw(w, h, board_x, board_y, cols, rows, snake, food, score, paused, game_over)

    gfx.end()


main()
