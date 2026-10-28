#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_gfx.h"
#include "solar_os_synth_voice.h"

#define SYNTH_APP_PIANO_BOTTOM_OFFSET 90
#define SYNTH_APP_PIANO_HEIGHT 63
#define SYNTH_APP_FOOTER_HEIGHT 21
typedef enum {
    SYNTH_CONTROL_WAVE, SYNTH_CONTROL_VOLUME, SYNTH_CONTROL_ATTACK,
    SYNTH_CONTROL_DECAY, SYNTH_CONTROL_SUSTAIN, SYNTH_CONTROL_RELEASE,
    SYNTH_CONTROL_COUNT
} synth_control_t;
static struct {
    bool keyboard_visible;
    uint8_t volume;
    synth_control_t selected;
    solar_os_synth_voice_config_t config;
} synth_app;
static const uint16_t synth_envelope_values[] = {0, 5, 80, 120, 10000};
struct solar_os_gfx {
    int width, height, bottom;
    unsigned piano, values;
};

static void record(solar_os_gfx_t *g, int x, int y, int w, int h) {
    assert(x >= 0 && y >= 0 && w > 0 && h > 0);
    assert(x + w <= g->width && y + h <= g->height);
    if (y + h - 1 > g->bottom) g->bottom = y + h - 1;
}
void solar_os_gfx_set_font(solar_os_gfx_t *g, solar_os_gfx_font_t font) {
    (void)g; (void)font;
}
size_t solar_os_gfx_text_width(solar_os_gfx_t *g, const char *text) {
    (void)g; return strlen(text) * 6;
}
void solar_os_gfx_text(solar_os_gfx_t *g, int x, int y, const char *text) {
    if (strncmp(text, "X keys", 6) == 0) return;
    record(g, x, y - 10, (int)solar_os_gfx_text_width(g, text), 13);
    if (strstr(text, "ms") || !strcmp(text, "100%")) g->values++;
}
void solar_os_gfx_rect(solar_os_gfx_t *g, int x, int y, int w, int h) {
    record(g, x, y, w, h);
}
void solar_os_gfx_line(solar_os_gfx_t *g, int x, int y, int x1, int y1) {
    record(g, x, y, 1, 1); record(g, x1, y1, 1, 1);
}
void solar_os_gfx_circle(solar_os_gfx_t *g, int x, int y, int r) {
    record(g, x - r, y - r, 2 * r + 1, 2 * r + 1);
}
static void synth_draw_wave_icon(solar_os_gfx_t *g, int x, int y, int w, int h,
    solar_os_synth_waveform_t wave, const solar_os_synth_voice_status_t *status) {
    (void)wave; (void)status; record(g, x, y, w + 1, h + 1);
}
static void synth_format_wave_status(const solar_os_synth_voice_status_t *status,
    char *text, size_t size) {
    (void)status; snprintf(text, size, "SQR abcd");
}
static void synth_draw_piano(solar_os_gfx_t *g, int x, int y, int w, int h) {
    assert(g->bottom < y);
    record(g, x, y, w, h); g->piano++;
}

#include "synth_play_view.inc"

int main(void) {
    const int sizes[][2] = {{400, 240}, {320, 240}, {400, 280}, {400, 300}, {800, 480}};
    synth_app.volume = 50;
    synth_app.config.attack_ms = synth_app.config.decay_ms = synth_app.config.release_ms = 10000;
    synth_app.config.sustain_percent = 100;
    solar_os_synth_voice_status_t status = {0};
    for (size_t size = 0; size < sizeof(sizes) / sizeof(sizes[0]); size++) {
        assert(!synth_use_compact_layout(sizes[size][0], sizes[size][1]));
        for (unsigned state = 0; state < 2; state++) {
            const bool keys = state == 0;
            synth_app.keyboard_visible = keys;
            for (unsigned selected = 0; selected < SYNTH_CONTROL_COUNT; selected++) {
                solar_os_gfx_t gfx = {.width=sizes[size][0], .height=sizes[size][1]};
                synth_app.selected = selected;
                synth_draw_play(&gfx, gfx.width, gfx.height, &status);
                assert(gfx.values == 4 && gfx.piano == keys);
                if (!keys) assert(gfx.bottom < gfx.height - SYNTH_APP_FOOTER_HEIGHT);
            }
        }
    }
    puts("Synth full Play view: controls, values and piano fit scaled and native canvases");
    return 0;
}
