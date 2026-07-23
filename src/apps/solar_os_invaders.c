#include "solar_os_invaders.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_log.h"
#include "freertos/FreeRTOS.h"
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
#include "freertos/queue.h"
#include "freertos/task.h"
#include "solar_os_audio.h"
#include "solar_os_queue.h"
#include "solar_os_task.h"
#endif
#include "solar_os_gfx.h"
#include "solar_os_keys.h"

#define INVADERS_ROWS 4
#define INVADERS_COLS 9
#define INVADERS_ENEMY_BULLETS 6
#define INVADERS_SHIELDS 4
#define INVADERS_SHIELD_COLS 6
#define INVADERS_SHIELD_ROWS 3
#define INVADERS_SHIELD_BLOCK 6

#define INVADERS_INVADER_W 16
#define INVADERS_INVADER_H 10
#define INVADERS_CELL_W 24
#define INVADERS_CELL_H 18
#define INVADERS_PLAYER_W 26
#define INVADERS_PLAYER_H 9

#define INVADERS_UPDATE_MS 50U
#define INVADERS_PLAYER_STEP 8
#define INVADERS_PLAYER_HOLD_MS 170U
#define INVADERS_SHOT_SPEED 9
#define INVADERS_BULLET_SPEED 5
#define INVADERS_STEP_X 5
#define INVADERS_STEP_Y 10
#define INVADERS_FIRE_MS 850U
#define INVADERS_SOUND_QUEUE_LEN 8
#define INVADERS_SOUND_STACK 8192
#define INVADERS_SOUND_STOP_MS 2000U

typedef enum {
    INVADERS_MODE_PLAYING,
    INVADERS_MODE_GAME_OVER,
    INVADERS_MODE_WON,
} invaders_mode_t;

typedef enum {
    INVADERS_SOUND_FIRE,
    INVADERS_SOUND_HIT,
    INVADERS_SOUND_PLAYER_HIT,
    INVADERS_SOUND_WIN,
    INVADERS_SOUND_GAME_OVER,
} invaders_sound_t;

typedef struct {
    bool active;
    int x;
    int y;
} invaders_bullet_t;

typedef struct {
    invaders_mode_t mode;
    bool alive[INVADERS_ROWS][INVADERS_COLS];
    uint8_t shield[INVADERS_SHIELDS][INVADERS_SHIELD_ROWS][INVADERS_SHIELD_COLS];
    invaders_bullet_t shot;
    invaders_bullet_t enemy_bullets[INVADERS_ENEMY_BULLETS];
    int formation_x;
    int formation_y;
    int formation_dir;
    int player_x;
    int player_dir;
    uint8_t lives;
    uint32_t score;
    uint32_t rng;
    uint32_t last_update_ms;
    uint32_t last_step_ms;
    uint32_t last_fire_ms;
    uint32_t move_until_ms;
    bool timers_started;
    bool phase;
} invaders_state_t;

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
typedef struct {
    QueueHandle_t queue;
    TaskHandle_t task;
    volatile bool stop_requested;
    volatile bool task_done;
    bool disabled;
} invaders_audio_t;
#endif

static const char *TAG = "solar_os_invaders";
static invaders_state_t invaders;
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
static invaders_audio_t invaders_audio;

static void invaders_sound_task(void *arg)
{
    (void)arg;
    invaders_audio.task_done = false;

    while (!invaders_audio.stop_requested) {
        invaders_sound_t sound;
        if (xQueueReceive(invaders_audio.queue, &sound, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (invaders_audio.stop_requested) {
            break;
        }
        if (invaders_audio.disabled) {
            continue;
        }

        esp_err_t err = ESP_OK;
        switch (sound) {
        case INVADERS_SOUND_FIRE:
            err = solar_os_audio_play_tone(1400, 28, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
            break;
        case INVADERS_SOUND_HIT:
            err = solar_os_audio_play_tone(420, 45, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
            break;
        case INVADERS_SOUND_PLAYER_HIT:
            err = solar_os_audio_play_tone(180, 80, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
            break;
        case INVADERS_SOUND_WIN:
            err = solar_os_audio_play_tone(880, 55, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
            if (err == ESP_OK) {
                err = solar_os_audio_play_tone(1320, 70, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
            }
            break;
        case INVADERS_SOUND_GAME_OVER:
            err = solar_os_audio_play_tone(220, 80, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
            if (err == ESP_OK) {
                err = solar_os_audio_play_tone(120, 110, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
            }
            break;
        default:
            break;
        }
        if (err != ESP_OK) {
            invaders_audio.disabled = true;
            SOLAR_OS_LOGW(TAG, "sound disabled: %s", esp_err_to_name(err));
        }
    }

    invaders_audio.task_done = true;
    solar_os_task_delete_internal(NULL);
}

static void invaders_sound_start(void)
{
    if (invaders_audio.task != NULL && !invaders_audio.task_done) {
        return;
    }
    if (invaders_audio.queue != NULL) {
        solar_os_queue_delete_internal(invaders_audio.queue);
    }

    memset(&invaders_audio, 0, sizeof(invaders_audio));
    invaders_audio.task_done = true;
    invaders_audio.queue = solar_os_queue_create_internal(
        INVADERS_SOUND_QUEUE_LEN,
        sizeof(invaders_sound_t));
    if (invaders_audio.queue == NULL) {
        SOLAR_OS_LOGW(TAG, "sound queue create failed");
        return;
    }

    invaders_audio.task_done = false;
    const BaseType_t created = solar_os_task_create_pinned_internal(
        invaders_sound_task,
        "invaders_sound",
        INVADERS_SOUND_STACK,
        NULL,
        tskIDLE_PRIORITY + 1,
        &invaders_audio.task,
        tskNO_AFFINITY);
    if (created != pdPASS) {
        SOLAR_OS_LOGW(TAG, "sound task create failed");
        solar_os_queue_delete_internal(invaders_audio.queue);
        memset(&invaders_audio, 0, sizeof(invaders_audio));
    }
}

static void invaders_sound_stop(void)
{
    if (invaders_audio.task == NULL) {
        if (invaders_audio.queue != NULL) {
            solar_os_queue_delete_internal(invaders_audio.queue);
        }
        memset(&invaders_audio, 0, sizeof(invaders_audio));
        return;
    }

    invaders_audio.stop_requested = true;
    if (invaders_audio.queue != NULL) {
        invaders_sound_t sound = INVADERS_SOUND_FIRE;
        (void)xQueueSend(invaders_audio.queue, &sound, 0);
    }
    const bool stopped = solar_os_task_wait_done(invaders_audio.task,
                                                &invaders_audio.task_done,
                                                INVADERS_SOUND_STOP_MS);
    if (!stopped) {
        SOLAR_OS_LOGW(TAG, "sound task did not stop within %u ms", INVADERS_SOUND_STOP_MS);
        return;
    }

    if (invaders_audio.queue != NULL) {
        solar_os_queue_delete_internal(invaders_audio.queue);
    }
    memset(&invaders_audio, 0, sizeof(invaders_audio));
}

static void invaders_sound_queue(invaders_sound_t sound)
{
    if (invaders_audio.queue == NULL || invaders_audio.disabled) {
        return;
    }
    (void)xQueueSend(invaders_audio.queue, &sound, 0);
}
#else
static void invaders_sound_start(void)
{
}

static void invaders_sound_stop(void)
{
}

static void invaders_sound_queue(invaders_sound_t sound)
{
    (void)sound;
}
#endif

static int invaders_screen_w(solar_os_gfx_t *gfx)
{
    return (int)solar_os_gfx_width(gfx);
}

static int invaders_screen_h(solar_os_gfx_t *gfx)
{
    return (int)solar_os_gfx_height(gfx);
}

static int invaders_formation_w(void)
{
    return ((INVADERS_COLS - 1) * INVADERS_CELL_W) + INVADERS_INVADER_W;
}

static int invaders_formation_h(void)
{
    return ((INVADERS_ROWS - 1) * INVADERS_CELL_H) + INVADERS_INVADER_H;
}

static int invaders_player_y(solar_os_gfx_t *gfx)
{
    return invaders_screen_h(gfx) - 20;
}

static int invaders_shield_y(solar_os_gfx_t *gfx)
{
    return invaders_screen_h(gfx) - 76;
}

static int invaders_clamp(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static bool invaders_rects_intersect(int ax,
                                     int ay,
                                     int aw,
                                     int ah,
                                     int bx,
                                     int by,
                                     int bw,
                                     int bh)
{
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

static uint32_t invaders_rand(void)
{
    invaders.rng = (invaders.rng * 1664525U) + 1013904223U;
    return invaders.rng;
}

static void invaders_enemy_rect(int row, int col, int *x, int *y)
{
    *x = invaders.formation_x + (col * INVADERS_CELL_W);
    *y = invaders.formation_y + (row * INVADERS_CELL_H);
}

static int invaders_alive_count(void)
{
    int count = 0;
    for (int row = 0; row < INVADERS_ROWS; row++) {
        for (int col = 0; col < INVADERS_COLS; col++) {
            if (invaders.alive[row][col]) {
                count++;
            }
        }
    }
    return count;
}

static void invaders_reset_shields(void)
{
    for (int shield = 0; shield < INVADERS_SHIELDS; shield++) {
        for (int row = 0; row < INVADERS_SHIELD_ROWS; row++) {
            for (int col = 0; col < INVADERS_SHIELD_COLS; col++) {
                const bool top_corner = row == 0 && (col == 0 || col == INVADERS_SHIELD_COLS - 1);
                const bool lower_notch = row == INVADERS_SHIELD_ROWS - 1 &&
                    (col == 2 || col == 3);
                invaders.shield[shield][row][col] = (top_corner || lower_notch) ? 0 : 2;
            }
        }
    }
}

static void invaders_reset(solar_os_gfx_t *gfx, uint32_t now_ms)
{
    memset(&invaders, 0, sizeof(invaders));
    invaders.mode = INVADERS_MODE_PLAYING;
    invaders.lives = 3;
    invaders.formation_dir = 1;
    invaders.rng = 0x51f15eadu ^ now_ms;
    invaders.last_update_ms = now_ms;
    invaders.last_step_ms = now_ms;
    invaders.last_fire_ms = now_ms;
    invaders.player_x = (invaders_screen_w(gfx) - INVADERS_PLAYER_W) / 2;
    invaders.formation_x = (invaders_screen_w(gfx) - invaders_formation_w()) / 2;
    invaders.formation_y = invaders_screen_h(gfx) > 220 ? 38 : 28;

    for (int row = 0; row < INVADERS_ROWS; row++) {
        for (int col = 0; col < INVADERS_COLS; col++) {
            invaders.alive[row][col] = true;
        }
    }

    invaders_reset_shields();
}

static void invaders_start_timers(uint32_t now_ms)
{
    invaders.timers_started = true;
    invaders.last_update_ms = now_ms;
    invaders.last_step_ms = now_ms;
    invaders.last_fire_ms = now_ms;
}

static int invaders_shield_x(solar_os_gfx_t *gfx, int shield)
{
    const int shield_w = INVADERS_SHIELD_COLS * INVADERS_SHIELD_BLOCK;
    const int total_w = (INVADERS_SHIELDS * shield_w) + ((INVADERS_SHIELDS - 1) * 32);
    const int start_x = (invaders_screen_w(gfx) - total_w) / 2;
    return start_x + shield * (shield_w + 32);
}

static bool invaders_hit_shield(solar_os_gfx_t *gfx, int x, int y, int width, int height)
{
    const int shield_y = invaders_shield_y(gfx);
    const int block = INVADERS_SHIELD_BLOCK;

    for (int shield = 0; shield < INVADERS_SHIELDS; shield++) {
        const int shield_x = invaders_shield_x(gfx, shield);
        for (int row = 0; row < INVADERS_SHIELD_ROWS; row++) {
            for (int col = 0; col < INVADERS_SHIELD_COLS; col++) {
                if (invaders.shield[shield][row][col] == 0) {
                    continue;
                }
                const int bx = shield_x + col * block;
                const int by = shield_y + row * block;
                if (invaders_rects_intersect(x, y, width, height, bx, by, block, block)) {
                    invaders.shield[shield][row][col]--;
                    return true;
                }
            }
        }
    }

    return false;
}

static uint32_t invaders_step_interval_ms(void)
{
    const int missing = (INVADERS_ROWS * INVADERS_COLS) - invaders_alive_count();
    int interval = 620 - (missing * 13);
    if (interval < 120) {
        interval = 120;
    }
    return (uint32_t)interval;
}

static void invaders_fire_player(solar_os_gfx_t *gfx)
{
    if (invaders.mode != INVADERS_MODE_PLAYING || invaders.shot.active) {
        return;
    }

    invaders.shot.active = true;
    invaders.shot.x = invaders.player_x + (INVADERS_PLAYER_W / 2);
    invaders.shot.y = invaders_player_y(gfx) - 3;
    invaders_sound_queue(INVADERS_SOUND_FIRE);
}

static void invaders_move_player(solar_os_gfx_t *gfx, int dir)
{
    const int max_x = invaders_screen_w(gfx) - INVADERS_PLAYER_W - 2;
    invaders.player_x = invaders_clamp(invaders.player_x + dir * INVADERS_PLAYER_STEP, 2, max_x);
}

static void invaders_spawn_enemy_bullet(void)
{
    const int alive = invaders_alive_count();
    if (alive <= 0) {
        return;
    }

    int slot = -1;
    for (int i = 0; i < INVADERS_ENEMY_BULLETS; i++) {
        if (!invaders.enemy_bullets[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return;
    }

    int pick = (int)(invaders_rand() % (uint32_t)alive);
    for (int row = INVADERS_ROWS - 1; row >= 0; row--) {
        for (int col = 0; col < INVADERS_COLS; col++) {
            if (!invaders.alive[row][col]) {
                continue;
            }
            if (pick-- == 0) {
                int x;
                int y;
                invaders_enemy_rect(row, col, &x, &y);
                invaders.enemy_bullets[slot].active = true;
                invaders.enemy_bullets[slot].x = x + (INVADERS_INVADER_W / 2);
                invaders.enemy_bullets[slot].y = y + INVADERS_INVADER_H + 2;
                return;
            }
        }
    }
}

static void invaders_step_formation(solar_os_gfx_t *gfx)
{
    int left = invaders_screen_w(gfx);
    int right = 0;
    bool any = false;

    for (int row = 0; row < INVADERS_ROWS; row++) {
        for (int col = 0; col < INVADERS_COLS; col++) {
            if (!invaders.alive[row][col]) {
                continue;
            }
            int x;
            int y;
            invaders_enemy_rect(row, col, &x, &y);
            if (x < left) {
                left = x;
            }
            if (x + INVADERS_INVADER_W > right) {
                right = x + INVADERS_INVADER_W;
            }
            any = true;
        }
    }

    if (!any) {
        invaders.mode = INVADERS_MODE_WON;
        invaders_sound_queue(INVADERS_SOUND_WIN);
        return;
    }

    const int next_left = left + invaders.formation_dir * INVADERS_STEP_X;
    const int next_right = right + invaders.formation_dir * INVADERS_STEP_X;
    if (next_left < 2 || next_right > invaders_screen_w(gfx) - 2) {
        invaders.formation_dir = -invaders.formation_dir;
        invaders.formation_y += INVADERS_STEP_Y;
        invaders.phase = !invaders.phase;
    } else {
        invaders.formation_x += invaders.formation_dir * INVADERS_STEP_X;
        invaders.phase = !invaders.phase;
    }

    if (invaders.formation_y + invaders_formation_h() >= invaders_player_y(gfx) - 4) {
        invaders.mode = INVADERS_MODE_GAME_OVER;
        invaders_sound_queue(INVADERS_SOUND_GAME_OVER);
    }
}

static void invaders_update_player_shot(solar_os_gfx_t *gfx)
{
    if (!invaders.shot.active) {
        return;
    }

    invaders.shot.y -= INVADERS_SHOT_SPEED;
    if (invaders.shot.y < 0) {
        invaders.shot.active = false;
        return;
    }

    if (invaders_hit_shield(gfx, invaders.shot.x - 1, invaders.shot.y - 5, 3, 7)) {
        invaders.shot.active = false;
        return;
    }

    for (int row = 0; row < INVADERS_ROWS; row++) {
        for (int col = 0; col < INVADERS_COLS; col++) {
            if (!invaders.alive[row][col]) {
                continue;
            }

            int x;
            int y;
            invaders_enemy_rect(row, col, &x, &y);
            if (invaders_rects_intersect(invaders.shot.x - 1,
                                         invaders.shot.y - 5,
                                         3,
                                         7,
                                         x,
                                         y,
                                         INVADERS_INVADER_W,
                                         INVADERS_INVADER_H)) {
                invaders.alive[row][col] = false;
                invaders.shot.active = false;
                invaders.score += (uint32_t)((INVADERS_ROWS - row) * 10);
                if (invaders_alive_count() == 0) {
                    invaders.mode = INVADERS_MODE_WON;
                    invaders_sound_queue(INVADERS_SOUND_WIN);
                } else {
                    invaders_sound_queue(INVADERS_SOUND_HIT);
                }
                return;
            }
        }
    }
}

static void invaders_reset_after_hit(solar_os_gfx_t *gfx)
{
    for (int i = 0; i < INVADERS_ENEMY_BULLETS; i++) {
        invaders.enemy_bullets[i].active = false;
    }
    invaders.shot.active = false;
    invaders.player_x = (invaders_screen_w(gfx) - INVADERS_PLAYER_W) / 2;
    invaders.player_dir = 0;
}

static void invaders_update_enemy_bullets(solar_os_gfx_t *gfx)
{
    const int player_y = invaders_player_y(gfx);
    const int screen_h = invaders_screen_h(gfx);

    for (int i = 0; i < INVADERS_ENEMY_BULLETS; i++) {
        if (!invaders.enemy_bullets[i].active) {
            continue;
        }

        invaders.enemy_bullets[i].y += INVADERS_BULLET_SPEED;
        const int x = invaders.enemy_bullets[i].x;
        const int y = invaders.enemy_bullets[i].y;

        if (invaders_hit_shield(gfx, x - 1, y, 3, 7)) {
            invaders.enemy_bullets[i].active = false;
            continue;
        }

        if (invaders_rects_intersect(x - 1,
                                     y,
                                     3,
                                     7,
                                     invaders.player_x,
                                     player_y,
                                     INVADERS_PLAYER_W,
                                     INVADERS_PLAYER_H)) {
            invaders.enemy_bullets[i].active = false;
            if (invaders.lives > 0) {
                invaders.lives--;
            }
            if (invaders.lives == 0) {
                invaders.mode = INVADERS_MODE_GAME_OVER;
                invaders_sound_queue(INVADERS_SOUND_GAME_OVER);
            } else {
                invaders_sound_queue(INVADERS_SOUND_PLAYER_HIT);
                invaders_reset_after_hit(gfx);
            }
            continue;
        }

        if (y > screen_h) {
            invaders.enemy_bullets[i].active = false;
        }
    }
}

static void invaders_update(solar_os_gfx_t *gfx, uint32_t now_ms)
{
    invaders.last_update_ms = now_ms;
    if (invaders.mode != INVADERS_MODE_PLAYING) {
        return;
    }

    if (invaders.player_dir != 0 && now_ms <= invaders.move_until_ms) {
        invaders_move_player(gfx, invaders.player_dir);
    } else {
        invaders.player_dir = 0;
    }

    invaders_update_player_shot(gfx);
    invaders_update_enemy_bullets(gfx);

    if (now_ms - invaders.last_step_ms >= invaders_step_interval_ms()) {
        invaders.last_step_ms = now_ms;
        invaders_step_formation(gfx);
    }

    if (now_ms - invaders.last_fire_ms >= INVADERS_FIRE_MS) {
        invaders.last_fire_ms = now_ms;
        invaders_spawn_enemy_bullet();
    }
}

static void invaders_draw_enemy(solar_os_gfx_t *gfx, int x, int y, int row)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    if (row == 0) {
        solar_os_gfx_fill_rect(gfx, x + 5, y + 1, 6, 2);
        solar_os_gfx_fill_rect(gfx, x + 3, y + 3, 10, 5);
        solar_os_gfx_fill_rect(gfx, x + 1, y + 5, 14, 2);
    } else if (row == 1) {
        solar_os_gfx_fill_rect(gfx, x + 4, y + 1, 8, 2);
        solar_os_gfx_fill_rect(gfx, x + 2, y + 3, 12, 6);
        solar_os_gfx_fill_rect(gfx, x, y + 5, 16, 2);
    } else {
        solar_os_gfx_fill_rect(gfx, x + 3, y + 1, 10, 2);
        solar_os_gfx_fill_rect(gfx, x + 1, y + 3, 14, 5);
        solar_os_gfx_fill_rect(gfx, x + 3, y + 8, 2, 2);
        solar_os_gfx_fill_rect(gfx, x + 11, y + 8, 2, 2);
    }

    if (invaders.phase) {
        solar_os_gfx_fill_rect(gfx, x + 1, y + 9, 3, 1);
        solar_os_gfx_fill_rect(gfx, x + 12, y + 9, 3, 1);
    } else {
        solar_os_gfx_fill_rect(gfx, x + 5, y + 9, 2, 1);
        solar_os_gfx_fill_rect(gfx, x + 9, y + 9, 2, 1);
    }

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x + 5, y + 4, 2, 2);
    solar_os_gfx_fill_rect(gfx, x + 10, y + 4, 2, 2);
}

static void invaders_draw_player(solar_os_gfx_t *gfx)
{
    const int y = invaders_player_y(gfx);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, invaders.player_x + 11, y, 4, 3);
    solar_os_gfx_fill_rect(gfx, invaders.player_x + 5, y + 3, 16, 3);
    solar_os_gfx_fill_rect(gfx, invaders.player_x, y + 6, INVADERS_PLAYER_W, 3);
}

static void invaders_draw_shields(solar_os_gfx_t *gfx)
{
    const int y = invaders_shield_y(gfx);
    for (int shield = 0; shield < INVADERS_SHIELDS; shield++) {
        const int x = invaders_shield_x(gfx, shield);
        for (int row = 0; row < INVADERS_SHIELD_ROWS; row++) {
            for (int col = 0; col < INVADERS_SHIELD_COLS; col++) {
                const uint8_t hp = invaders.shield[shield][row][col];
                if (hp == 0) {
                    continue;
                }
                solar_os_gfx_set_color(gfx, hp > 1 ? SOLAR_OS_GFX_COLOR_BLACK : SOLAR_OS_GFX_COLOR_DARK);
                solar_os_gfx_fill_rect(gfx,
                                       x + col * INVADERS_SHIELD_BLOCK,
                                       y + row * INVADERS_SHIELD_BLOCK,
                                       INVADERS_SHIELD_BLOCK - 1,
                                       INVADERS_SHIELD_BLOCK - 1);
            }
        }
    }
}

static void invaders_draw_bullets(solar_os_gfx_t *gfx)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    if (invaders.shot.active) {
        solar_os_gfx_fill_rect(gfx, invaders.shot.x - 1, invaders.shot.y - 6, 3, 7);
    }

    for (int i = 0; i < INVADERS_ENEMY_BULLETS; i++) {
        if (!invaders.enemy_bullets[i].active) {
            continue;
        }
        const int x = invaders.enemy_bullets[i].x;
        const int y = invaders.enemy_bullets[i].y;
        solar_os_gfx_fill_rect(gfx, x - 1, y, 3, 7);
    }
}

static void invaders_draw_header(solar_os_gfx_t *gfx)
{
    char text[64];
    snprintf(text, sizeof(text), "SCORE %lu   LIVES %u", (unsigned long)invaders.score, invaders.lives);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_text(gfx, 4, 12, text);
}

static void invaders_draw_center_text(solar_os_gfx_t *gfx, const char *text)
{
    const int screen_w = invaders_screen_w(gfx);
    const int screen_h = invaders_screen_h(gfx);
    int width = 0;
    for (const char *p = text; *p != '\0'; p++) {
        width += 8;
    }
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_text(gfx, (screen_w - width) / 2, screen_h / 2, text);
}

static void invaders_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    invaders_draw_header(gfx);

    for (int row = 0; row < INVADERS_ROWS; row++) {
        for (int col = 0; col < INVADERS_COLS; col++) {
            if (!invaders.alive[row][col]) {
                continue;
            }
            int x;
            int y;
            invaders_enemy_rect(row, col, &x, &y);
            invaders_draw_enemy(gfx, x, y, row);
        }
    }

    invaders_draw_shields(gfx);
    invaders_draw_bullets(gfx);
    invaders_draw_player(gfx);

    if (invaders.mode == INVADERS_MODE_GAME_OVER) {
        invaders_draw_center_text(gfx, "GAME OVER");
    } else if (invaders.mode == INVADERS_MODE_WON) {
        invaders_draw_center_text(gfx, "VICTORY");
    }

    solar_os_gfx_present(gfx);
}

static esp_err_t invaders_start(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_context_set_graphics_active(ctx, true);
    invaders_sound_start();
    invaders_reset(gfx, 0);
    invaders_render(ctx);
    return ESP_OK;
}

static void invaders_stop(solar_os_context_t *ctx)
{
    invaders_sound_stop();
    solar_os_context_set_graphics_active(ctx, false);
}

static bool invaders_handle_char(solar_os_context_t *ctx, uint8_t ch)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return true;
    }

    if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE) {
        solar_os_context_request_exit(ctx);
        return true;
    }

    if ((ch == SOLAR_OS_KEY_LEFT || ch == SOLAR_OS_KEY_CTRL_LEFT) &&
        invaders.mode == INVADERS_MODE_PLAYING) {
        invaders.player_dir = -1;
        invaders.move_until_ms = invaders.last_update_ms + INVADERS_PLAYER_HOLD_MS;
        invaders_move_player(gfx, -1);
        invaders_render(ctx);
        return true;
    }

    if ((ch == SOLAR_OS_KEY_RIGHT || ch == SOLAR_OS_KEY_CTRL_RIGHT) &&
        invaders.mode == INVADERS_MODE_PLAYING) {
        invaders.player_dir = 1;
        invaders.move_until_ms = invaders.last_update_ms + INVADERS_PLAYER_HOLD_MS;
        invaders_move_player(gfx, 1);
        invaders_render(ctx);
        return true;
    }

    if (ch == ' ' || ch == 'f' || ch == 'F') {
        if (invaders.mode == INVADERS_MODE_PLAYING) {
            invaders_fire_player(gfx);
        } else {
            invaders_reset(gfx, invaders.last_update_ms);
        }
        invaders_render(ctx);
        return true;
    }

    return true;
}

static bool invaders_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        return invaders_handle_char(ctx, (uint8_t)event->data.ch);
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) {
            return true;
        }
        if (!invaders.timers_started) {
            invaders_start_timers(event->data.tick_ms);
            return true;
        }
        if (event->data.tick_ms - invaders.last_update_ms >= INVADERS_UPDATE_MS) {
            invaders_update(gfx, event->data.tick_ms);
            invaders_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_RESUME) {
        invaders_render(ctx);
        return true;
    }

    return false;
}

const solar_os_app_t solar_os_invaders_app = {
    .name = "invaders",
    .summary = "arcade shooter",
    .start = invaders_start,
    .stop = invaders_stop,
    .event = invaders_event,
    .tick_interval_ms = INVADERS_UPDATE_MS,
    .tick_deadline_ms = 25U,
};
