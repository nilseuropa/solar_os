#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_gfx.h"
#include "solar_os_synth_voice.h"
#include "synth_types.inc"
#define SYNTH_APP_TWO_PI 6.28318530717958647692f
#define SYNTH_APP_FACTORY_PRESET_COUNT 8U
#define SYNTH_APP_USER_PRESET_COUNT 8U
#define SYNTH_APP_TOUCH_TARGETS_MAX 24U
static struct {
    synth_touch_target_t pointer_target, touch_targets[24];
    size_t touch_target_count;
    bool keyboard_visible, hold_mode;
    uint8_t volume, velocity, wave_brush;
    int octave;
    synth_tab_t tab;
    synth_control_t selected;
    synth_filter_control_t filter_selected;
    synth_oscillator2_control_t oscillator2_selected;
    synth_mode_control_t mode_selected;
    solar_os_synth_voice_config_t config;
    solar_os_synth_voice_performance_t performance;
    synth_wave_baseline_t baseline;
    int16_t wavetable[256];
    size_t wave_steps, wave_cursor, preset_selected;
    synth_preset_slot_state_t preset_slots[8];
    char preset_message[40];
    esp_err_t last_error;
} synth_app;
static const uint16_t synth_envelope_values[] = {0, 5, 80, 120, 10000};
static const uint16_t synth_filter_cutoff_values[] = {40, 1000, 18000};
static const char *const synth_factory_preset_names[8] = {
    "INIT SQUARE", "INIT TRIANGLE", "INIT SAW", "SOFT SINE",
    "ANALOG BASS", "POLY BRASS", "SYNTH STRINGS", "DIGI E.PIANO"};
struct solar_os_gfx {
    int width, height, bottom;
    solar_os_gfx_font_t font;
    bool in_bottom;
    unsigned bottom_draws;
    int last_circle_x, last_circle_y, last_circle_radius;
    unsigned selected_knobs;
};
size_t solar_os_gfx_width(const solar_os_gfx_t *g) { return g->width; }
static void record(solar_os_gfx_t *g, int x, int y, int w, int h) {
    if (!(x >= 0 && y >= 0 && w > 0 && h > 0 && x+w <= g->width && y+h <= g->height)) {
        fprintf(stderr,"tab=%d keys=%d %dx%d primitive=(%d,%d,%d,%d)\n",
            synth_app.tab,synth_app.keyboard_visible,g->width,g->height,x,y,w,h);
        assert(0);
    }
    if (!g->in_bottom && y + h - 1 > g->bottom) g->bottom = y + h - 1;
}
void solar_os_gfx_set_color(solar_os_gfx_t *g, solar_os_gfx_color_t color) { (void)g; (void)color; }
void solar_os_gfx_set_font(solar_os_gfx_t *g, solar_os_gfx_font_t font) { g->font = font; }
size_t solar_os_gfx_text_width(solar_os_gfx_t *g, const char *text) {
    unsigned width = g->font == SOLAR_OS_GFX_FONT_MONO_14 || g->font == SOLAR_OS_GFX_FONT_MONO_16 ||
        g->font == SOLAR_OS_GFX_FONT_BOLD_16 ? 8 : 6;
    return strlen(text) * width;
}
void solar_os_gfx_text(solar_os_gfx_t *g, int x, int y, const char *text) {
    assert(!strstr(text,"X keys") && !strstr(text,"Arrows") && !strstr(text,"PgUp"));
    if (!*text) return;
    record(g,x,y-10,(int)solar_os_gfx_text_width(g,text),13);
}
void solar_os_gfx_rect(solar_os_gfx_t *g, int x, int y, int w, int h) { record(g,x,y,w,h); }
void solar_os_gfx_fill_rect(solar_os_gfx_t *g, int x, int y, int w, int h) { record(g,x,y,w,h); }
void solar_os_gfx_pixel(solar_os_gfx_t *g, int x, int y) { record(g,x,y,1,1); }
void solar_os_gfx_line(solar_os_gfx_t *g, int x, int y, int x1, int y1) {
    record(g,x,y,1,1); record(g,x1,y1,1,1);
}
void solar_os_gfx_circle(solar_os_gfx_t *g, int x, int y, int r) {
    record(g,x-r,y-r,2*r+1,2*r+1);
    if(x==g->last_circle_x && y==g->last_circle_y && r==g->last_circle_radius+3)g->selected_knobs++;
    g->last_circle_x=x;g->last_circle_y=y;g->last_circle_radius=r;
}
const char *esp_err_to_name(esp_err_t err) { (void)err; return "ESP_FAIL"; }
static bool synth_semitone_held(uint8_t semitone) { return semitone == 0 || semitone == 1; }
static const char *synth_baseline_name(synth_wave_baseline_t b) { (void)b; return "SQUARE"; }
static const char *synth_wave_short_name(solar_os_synth_waveform_t w) { (void)w; return "SQR"; }
static void synth_draw_wave_icon(solar_os_gfx_t *g, int x, int y, int w, int h,
    solar_os_synth_waveform_t wave, const solar_os_synth_voice_status_t *status) {
    (void)wave; (void)status; record(g,x,y,w+1,h+1);
}
static void synth_format_wave_status(const solar_os_synth_voice_status_t *status, char *text, size_t size) {
    (void)status; snprintf(text,size,"SQR abcd");
}
static void synth_draw_bottom(solar_os_gfx_t *gfx, int width, int height);
#include "synth_play_view.inc"
static void synth_draw_bottom(solar_os_gfx_t *g, int width, int height) {
    const int top = height - synth_bottom_height(height) - 6;
    if (g->bottom >= top) fprintf(stderr,"tab %d body bottom=%d panel top=%d\n",synth_app.tab,g->bottom,top);
    assert(g->bottom < top);
    g->in_bottom=true; g->bottom_draws++;
    synth_draw_bottom_pixels(g,width,height);
    g->in_bottom=false;
}
int main(void) {
    const int sizes[][2] = {{240,200},{300,400},{480,320},{400,240},{320,240},{400,280},{400,300},{800,480}};
    synth_app.volume=50; synth_app.velocity=100; synth_app.octave=4;
    synth_app.wave_steps=16; synth_app.wave_cursor=8; synth_app.wave_brush=1;
    synth_app.config.attack_ms=synth_app.config.decay_ms=synth_app.config.release_ms=10000;
    synth_app.config.sustain_percent=100;
    synth_app.config.filter=(solar_os_synth_filter_config_t){18000,100,100,10000,10000,100,10000};
    solar_os_synth_voice_status_t status={.running=true,.sample_rate=48000,
        .pcm_sample_count=64,.pcm_peak=32768,.pcm_rms=18000};
    for (unsigned i=0;i<64;i++) status.pcm_samples[i]=(i&1) ? -32768 : 32767;
    for (size_t size=0;size<sizeof(sizes)/sizeof(sizes[0]);size++) {
        const int width=sizes[size][0],height=sizes[size][1];
        assert(!synth_use_compact_layout(width,height));
        for (unsigned keys=0;keys<2;keys++) for (synth_tab_t tab=0;tab<SYNTH_TAB_COUNT;tab++) {
            synth_app.keyboard_visible=keys; synth_app.tab=tab; synth_app.touch_target_count=0;
            solar_os_gfx_t gfx={.width=width,.height=height};
            synth_draw_header(&gfx,&status);
            switch (tab) {
            case SYNTH_TAB_PLAY: synth_draw_play(&gfx,width,height,&status); break;
            case SYNTH_TAB_FILTER: synth_draw_filter_editor(&gfx,width,height); break;
            case SYNTH_TAB_WAVE: synth_draw_wave_editor(&gfx,width,height); break;
            case SYNTH_TAB_OSCILLATOR2: synth_draw_oscillator2_editor(&gfx,width,height); break;
            case SYNTH_TAB_MODE: synth_draw_mode_editor(&gfx,width,height); break;
            case SYNTH_TAB_PRESET: synth_draw_preset_editor(&gfx,width,height); break;
            case SYNTH_TAB_SCOPE: synth_draw_scope(&gfx,width,height,&status); break;
            default: assert(0);
            }
            assert(gfx.bottom_draws==1);
            synth_touch_target_t target;
            assert(synth_touch_target_at(width-24,12,&target) && target.kind==SYNTH_TOUCH_KEYBOARD);
            for (size_t i=0; i<synth_app.touch_target_count; i++) {
                const solar_os_display_rect_t *r=&synth_app.touch_targets[i].rect;
                assert(r->x>=0 && r->y>=0 && r->width>0 && r->height>0);
                assert(r->x+r->width<=width && r->y+r->height<=height);
            }
            const int white[]={0,2,4,5,7,9,11,12};
            for (unsigned i=0;i<8;i++)
                assert(synth_piano_note_at(width,height,6+(2*i+1)*(width-12)/16,height-8)==white[i]);
            assert(synth_piano_note_at(width,height,0,height-8)==-1);
            assert(synth_piano_note_at(width,height,20,height-1)==-1);
            const int black[]={1,3,-1,6,8,10,-1};
            for(unsigned i=0;i<7;i++) if(black[i]>=0)
                assert(synth_piano_note_at(width,height,6+(i+1)*(width-12)/8,
                    height-synth_bottom_height(height)-4)==black[i]);
            for (unsigned button=0;button<SYNTH_APP_TAB_BUTTON_COUNT;button++) {
                solar_os_display_rect_t rect;
                assert(synth_tab_button_rect(width,height,button,&rect));
                assert(synth_tab_at(width,height,rect.x+rect.width/2,rect.y+rect.height/2)==(int)button);
                assert(synth_tab_at(width,height,rect.x,rect.y)==(int)button);
                assert(synth_tab_at(width,height,rect.x+rect.width-1,rect.y+rect.height-1)==(int)button);
            }
            if(width<560) {
                solar_os_display_rect_t first,last;
                assert(synth_tab_button_rect(width,height,0,&first));
                assert(synth_tab_button_rect(width,height,4,&last));
                assert(first.x==last.x && first.width==last.width);
            }
            assert(synth_tab_at(width,height,0,0)==-1);
            assert(synth_tab_at(width,height,width-1,height-1)==-1);
        }
    }
    for (unsigned running=0;running<2;running++) for (unsigned count=0;count<=65;count++) {
        status.running=running;status.pcm_sample_count=count;
        solar_os_gfx_t gfx={.width=128,.height=64};
        synth_draw_scope(&gfx,128,64,&status);
        assert(synth_use_micro_layout(64,32));
        assert(synth_piano_note_at(128,64,32,60)==-1);
    }
    for(unsigned first_oscillator=0;first_oscillator<2;first_oscillator++) {
        synth_app.tab=SYNTH_TAB_OSCILLATOR2;
        synth_app.pointer_target.kind=first_oscillator ? SYNTH_TOUCH_PLAY : SYNTH_TOUCH_NONE;
        synth_app.touch_target_count=0;
        synth_app.oscillator2_selected=SYNTH_OSCILLATOR2_CONTROL_WAVE;
        solar_os_gfx_t gfx={.width=480,.height=320};
        synth_draw_oscillator2_editor(&gfx,480,320);
        assert(gfx.selected_knobs==!first_oscillator);
    }
    puts("Synth: all seven tabs fit, bottom modes share geometry, buttons hit correctly, PCM trace bounds pass");
}
