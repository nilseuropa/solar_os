#include "solar_os_gfx.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(n)
struct solar_os_gfx {
    int width, height;
    solar_os_gfx_font_t font;
    unsigned color;
};
static uint8_t pixels[800 * 480];
size_t solar_os_gfx_width(const solar_os_gfx_t *g) { return g->width; }
size_t solar_os_gfx_height(const solar_os_gfx_t *g) { return g->height; }
void solar_os_gfx_set_font(solar_os_gfx_t *g, solar_os_gfx_font_t f) {
    g->font = f;
}
void solar_os_gfx_set_color(solar_os_gfx_t *g, solar_os_gfx_color_t c) {
    g->color = c;
}
size_t solar_os_gfx_text_width(solar_os_gfx_t *g, const char *s) {
    return strlen(s) * (g->font == SOLAR_OS_GFX_FONT_BITMAP_8 ? 5 : 7);
}
void solar_os_gfx_text(solar_os_gfx_t *g, int x, int y, const char *s) {
    assert(x >= 0 && y >= 7 &&
           x + (int)solar_os_gfx_text_width(g, s) <= g->width && y < g->height);
}
void solar_os_gfx_fill_rect(solar_os_gfx_t *g, int x, int y, int w, int h) {
    assert(x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= g->width &&
           y + h <= g->height);
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            pixels[row * g->width + col] = g->color != SOLAR_OS_GFX_COLOR_WHITE;
}
#include "invaders_geometry.inc"
int main(void) {
    const int screens[][2] = {{128, 64},  {240, 64},  {240, 135},
                              {384, 168}, {320, 240}, {400, 300}};
    for (unsigned screen = 0; screen < sizeof(screens) / sizeof(*screens);
         screen++) {
        solar_os_gfx_t g = {.width = screens[screen][0],
                            .height = screens[screen][1]};
        invaders_reset(&g, 0);
        invaders_start_timers(0);
        assert(invaders.formation_y + invaders_formation_h() <
               invaders_shield_y(&g));
        assert(invaders_shield_y(&g) + 18 < invaders_player_y(&g));
        invaders_update(&g, 50);
        assert(invaders.mode == INVADERS_MODE_PLAYING);
        for (int phase = 0; phase < 2; phase++) {
            invaders.phase = phase;
            memset(pixels, 0, sizeof(pixels));
            invaders_draw_header(&g);
            for (int row = 0; row < INVADERS_ROWS; row++)
                for (int col = 0; col < INVADERS_COLS; col++) {
                    unsigned before = 0, after = 0;
                    for (int k = 0; k < g.width * g.height; k++)
                        before += pixels[k];
                    int x, y;
                    invaders_enemy_rect(row, col, &x, &y);
                    invaders_draw_enemy(&g, x, y, row);
                    for (int k = 0; k < g.width * g.height; k++)
                        after += pixels[k];
                    assert(after >
                           before); /* Every scaled sprite remains visible. */
                }
            invaders_draw_shields(&g);
            invaders_draw_player(&g);
            invaders_draw_center_text(&g, "GAME OVER");
        }
        for (unsigned k = 0; k < 200; k++)
            invaders_move_player(&g, -1);
        assert(invaders.player_x == 2);
        for (unsigned k = 0; k < 200; k++)
            invaders_move_player(&g, 1);
        assert(invaders.player_x + INVADERS_PLAYER_W <=
               invaders_screen_w(&g) - 2);
        /* Collision and motion use the same playfield as the mapped sprites. */
        int x, y;
        invaders_enemy_rect(3, 4, &x, &y);
        invaders.shot =
            (invaders_bullet_t){true, x + 8, y + 5 + INVADERS_SHOT_SPEED};
        invaders_update_player_shot(&g);
        assert(!invaders.alive[3][4] && invaders.score == 10 &&
               !invaders.shot.active);
        uint8_t lives = invaders.lives;
        invaders.enemy_bullets[0] =
            (invaders_bullet_t){true, invaders.player_x + 13,
                                invaders_player_y(&g) - INVADERS_BULLET_SPEED};
        invaders_update_enemy_bullets(&g);
        assert(invaders.lives == lives - 1);
        invaders_fire_player(&g);
        invaders_draw_bullets(&g);
    }
    puts("Invaders small-screen sprite visibility, geometry, motion and "
         "collisions passed");
}
