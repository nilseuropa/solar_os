#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_gfx.h"
#include "solar_os_input.h"
#include "solar_os_synth_voice.h"
#define SYNTH_APP_OWNER "app:synth"
#define SYNTH_APP_TOUCH_TARGETS_MAX 24U
#define SYNTH_APP_HELD_MAX 16U
#define SYNTH_APP_PULSE_MS 220U
/* TYPES */
static struct {
    bool headless, suspended, keyboard_visible, pointer_pressed, hold_mode, compact_parameter_valid;
    int pointer_x, pointer_y, pointer_drag_y, pointer_note, octave;
    uint8_t velocity, pointer_id, wave_brush;
    solar_os_input_source_t pointer_source;
    synth_tab_t tab;
    synth_control_t selected;
    synth_filter_control_t filter_selected;
    synth_oscillator2_control_t oscillator2_selected;
    synth_mode_control_t mode_selected;
    solar_os_synth_voice_performance_t performance;
    size_t preset_selected, touch_target_count, wave_steps, wave_cursor;
    synth_touch_target_t pointer_target, touch_targets[24];
    synth_held_note_t held[16];
    int16_t wavetable[256];
    esp_err_t last_error;
} synth_app;
struct solar_os_gfx { int width, height; };
typedef struct { solar_os_gfx_t *gfx; } solar_os_context_t;
static solar_os_gfx_t *solar_os_context_gfx(solar_os_context_t *ctx) { return ctx->gfx; }
size_t solar_os_gfx_width(const solar_os_gfx_t *gfx) { return gfx->width; }
size_t solar_os_gfx_height(const solar_os_gfx_t *gfx) { return gfx->height; }
static uint32_t synth_now_ms(void) { return 1000; }
static unsigned render_count, on_count, off_count, snapshots, uploads, loads, saves, baselines, resolutions;
static uint32_t last_frequency;
static synth_touch_kind_t adjusted_kind;
static unsigned adjusted_control;
static int adjusted_steps;
static const uint16_t synth_note_frequencies_octave_4[] = {262,277,294,311,330,349,370,392,415,440,466,494,523};
static const uint16_t synth_note_usages[] = {4,26,22,8,7,9,23,10,28,11,24,13,14};
esp_err_t solar_os_synth_voice_note_on(const char *owner, uint32_t hz, uint8_t velocity) {
    assert(!strcmp(owner,SYNTH_APP_OWNER) && velocity);on_count++;last_frequency=hz;return ESP_OK;
}
esp_err_t solar_os_synth_voice_note_off(const char *owner, uint32_t hz) {
    assert(!strcmp(owner,SYNTH_APP_OWNER) && hz);off_count++;return ESP_OK;
}
static void synth_render_changed(solar_os_context_t *ctx, bool changed, bool performance, uint32_t now) {
    (void)ctx;(void)performance;(void)now;if(changed)render_count++;
}
static void synth_adjust_selected(int direction) {
    adjusted_kind=SYNTH_TOUCH_PLAY;adjusted_control=synth_app.selected;adjusted_steps+=direction;
}
static void synth_adjust_filter_selected(int direction) {
    adjusted_kind=SYNTH_TOUCH_FILTER;adjusted_control=synth_app.filter_selected;adjusted_steps+=direction;
}
static void synth_adjust_oscillator2_selected(int direction) {
    adjusted_kind=SYNTH_TOUCH_OSCILLATOR2;adjusted_control=synth_app.oscillator2_selected;adjusted_steps+=direction;
}
static void synth_adjust_mode_selected(int direction) {
    adjusted_kind=SYNTH_TOUCH_MODE;adjusted_control=synth_app.mode_selected;adjusted_steps+=direction;
    if(synth_app.mode_selected==SYNTH_MODE_CONTROL_VOICES)synth_app.performance.mono=direction>0;
}
static void synth_preset_load_selected(void) { loads++; }
static void synth_preset_save_selected(void) { saves++; }
static void synth_wavetable_snapshot(void) { snapshots++; }
static void synth_wavetable_commit(void) { uploads++; }
static bool synth_handle_wave_control(uint8_t key) { assert(key=='b');baselines++;return true; }
static void synth_wavetable_cycle_steps(void) { resolutions++; }
/* FUNCTIONS */
static bool pointer(solar_os_context_t *ctx, solar_os_input_pointer_action_t action, int x, int y) {
    const solar_os_input_pointer_event_t e={.source=2,.mode=SOLAR_OS_INPUT_POINTER_ABSOLUTE,
        .action=action,.x=x,.y=y,.buttons=action==SOLAR_OS_INPUT_POINTER_RELEASE?0:1};
    return synth_pointer_event(ctx,&e);
}
static void target(synth_touch_kind_t kind, unsigned control) {
    synth_app.touch_target_count=0;
    synth_add_touch_target(50,50,100,100,kind,control);
}
int main(void) {
    solar_os_gfx_t gfx={480,320};solar_os_context_t ctx={&gfx};
    synth_app.octave=4;synth_app.velocity=100;synth_app.pointer_note=-1;synth_app.keyboard_visible=true;
    synth_app.wave_steps=16;synth_app.wave_brush=1;
    assert(pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,20,310));
    assert(on_count==1 && last_frequency==262 && synth_app.held[0].release_at_ms==0);
    solar_os_input_pointer_event_t other={.source=3,.action=SOLAR_OS_INPUT_POINTER_RELEASE};
    assert(!synth_pointer_event(&ctx,&other) && synth_app.pointer_pressed);
    assert(pointer(&ctx,SOLAR_OS_INPUT_POINTER_MOVE,90,310));
    assert(on_count==2 && off_count==1 && last_frequency==294);
    assert(pointer(&ctx,SOLAR_OS_INPUT_POINTER_RELEASE,90,310));
    assert(off_count==2 && synth_app.pointer_note==-1 && !synth_app.pointer_pressed);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,20,310);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_MOVE,20,20);
    assert(off_count==3 && synth_app.pointer_note==-1);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_RELEASE,20,20);
    assert(synth_set_hold_mode(true));
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,20,310);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_RELEASE,20,310);
    assert(off_count==3 && synth_app.held[0].active);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,20,310);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_RELEASE,20,310);
    assert(off_count==4 && !synth_app.held[0].active);
    assert(synth_set_hold_mode(false));
    target(SYNTH_TOUCH_MODE,SYNTH_MODE_CONTROL_VOICES);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,75,75);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_RELEASE,75,75);
    assert(synth_app.performance.mono);
    target(SYNTH_TOUCH_MODE,SYNTH_MODE_CONTROL_HOLD);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,75,75);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_RELEASE,75,75);
    assert(synth_app.hold_mode);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,75,75);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_RELEASE,75,75);
    assert(!synth_app.hold_mode);
    const synth_touch_kind_t kinds[]={SYNTH_TOUCH_PLAY,SYNTH_TOUCH_FILTER,SYNTH_TOUCH_OSCILLATOR2,SYNTH_TOUCH_MODE};
    const unsigned controls[]={SYNTH_CONTROL_ATTACK,SYNTH_FILTER_CONTROL_RESONANCE,
        SYNTH_OSCILLATOR2_CONTROL_DETUNE,SYNTH_MODE_CONTROL_GLIDE};
    for(unsigned i=0;i<4;i++) {
        target(kinds[i],controls[i]);adjusted_steps=0;
        pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,75,100);
        assert(adjusted_steps==0);
        pointer(&ctx,SOLAR_OS_INPUT_POINTER_MOVE,75,88);
        assert(adjusted_kind==kinds[i] && adjusted_control==controls[i] && adjusted_steps==3);
        pointer(&ctx,SOLAR_OS_INPUT_POINTER_RELEASE,75,88);
        pointer(&ctx,SOLAR_OS_INPUT_POINTER_MOVE,75,60);
        assert(adjusted_steps==3);
    }
    target(SYNTH_TOUCH_KEYBOARD,0);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,75,75);
    assert(!synth_app.keyboard_visible && !synth_app.pointer_pressed);
    synth_app.touch_target_count=0;
    for(synth_tab_t tab=0;tab<SYNTH_TAB_COUNT;tab++) {
        solar_os_display_rect_t r;assert(synth_tab_button_rect(480,320,tab,&r));
        pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,r.x+r.width/2,r.y+r.height/2);
        assert(synth_app.tab==tab);
        pointer(&ctx,SOLAR_OS_INPUT_POINTER_RELEASE,r.x+r.width/2,r.y+r.height/2);
    }
    solar_os_display_rect_t keys;
    assert(synth_tab_button_rect(480,320,SYNTH_TAB_COUNT,&keys));
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,keys.x+keys.width/2,keys.y+keys.height/2);
    assert(synth_app.keyboard_visible && synth_app.tab==SYNTH_TAB_SCOPE);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_RELEASE,keys.x+keys.width/2,keys.y+keys.height/2);
    target(SYNTH_TOUCH_PRESET,11);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,75,75);pointer(&ctx,SOLAR_OS_INPUT_POINTER_RELEASE,75,75);
    assert(synth_app.preset_selected==11 && !loads && !saves);
    target(SYNTH_TOUCH_PRESET_LOAD,0);pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,75,75);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_RELEASE,75,75);assert(loads==1);
    target(SYNTH_TOUCH_PRESET_SAVE,0);pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,75,75);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_RELEASE,75,75);assert(saves==1);
    target(SYNTH_TOUCH_WAVE_BASELINE,0);pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,75,75);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_RELEASE,75,75);assert(baselines==1);
    target(SYNTH_TOUCH_WAVE_STEPS,0);pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,75,75);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_RELEASE,75,75);assert(resolutions==1);
    target(SYNTH_TOUCH_WAVE_GRAPH,0);pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,75,50);
    assert(snapshots==1 && uploads==1);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_MOVE,120,-30000);
    assert(snapshots==1 && uploads==2);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_MOVE,30000,30000);
    assert(snapshots==1 && uploads==3 && synth_app.wave_cursor==15);
    pointer(&ctx,SOLAR_OS_INPUT_POINTER_RELEASE,30000,30000);
    synth_app.headless=true;unsigned before=render_count;
    assert(!pointer(&ctx,SOLAR_OS_INPUT_POINTER_PRESS,75,75) && render_count==before);
    puts("Synth pointer: selectable controls, drag routing, piano slide/release/hold, tabs, presets and wave strokes passed");
}
