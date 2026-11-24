/* Exercise native app polling and session focus against the real input service. */
#define main input_service_tests
#include "input_test.c"
#undef main
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "solar_os_native_app_abi.h"
#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 16U

static struct {char input_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];} session_state;
static int input_focus_lock;
typedef struct {char name[16]; bool ready; void *u8g2;} solar_os_display_target_t;
static bool solar_os_display_find_target(const char *name, solar_os_display_target_t *target)
{
    if (strcmp(name, "left") && strcmp(name, "right")) return false;
    strlcpy(target->name, name, sizeof(target->name)); target->ready = true; target->u8g2 = target;
    return true;
}
static const void *session_active_for_display(const char *name) {return name;}
static unsigned owner_operations;
static esp_err_t solar_os_sessions_run_on_owner(esp_err_t (*operation)(void *), void *context)
{ owner_operations++; return operation(context); }
#include "session_focus.inc"
static bool solar_os_sessions_input_focus(char *out, size_t size)
{ return session_copy_input_focus(out, size); }
static bool solar_os_gfx_display_target_name(const void *gfx, char *out, size_t size)
{ if (!gfx) return false; strlcpy(out, gfx, size); return true; }
typedef struct {solar_os_native_foreground_key_v1_t keys[16]; unsigned count;} key_queue_t;
typedef struct {
    const void *gfx;
    key_queue_t *keys;
    bool keyboard_captured;
    atomic_bool input_reset;
} foreground_state_t;
static void xQueueReset(key_queue_t *queue) { queue->count = 0; }
static void foreground_queue_key(foreground_state_t *state, solar_os_native_foreground_key_v1_t *key)
{ assert(state->keys->count < 16); state->keys->keys[state->keys->count++] = *key; }
#include "native_focus.inc"

int main(void)
{
    assert(solar_os_input_init() == ESP_OK);
    solar_os_input_source_t source;
    assert(solar_os_input_keyboard_source_open("keyboard", true, &source) == ESP_OK);
    key_queue_t right_keys = {0}, left_keys = {0};
    foreground_state_t right = {.gfx = "right", .keys = &right_keys};
    foreground_state_t left = {.gfx = "left", .keys = &left_keys};
    assert(solar_os_sessions_focus_display("left") == ESP_OK && owner_operations == 1);
    foreground_poll_keyboard(&right);
    assert(!right.keyboard_captured);

    assert(solar_os_sessions_focus_display("right") == ESP_OK && owner_operations == 2);
    foreground_poll_keyboard(&right);
    assert(right.keyboard_captured && solar_os_input_keyboard_captured_by(&right));
    session_store_input_focus("right"); /* Reassigning the same focus keeps its claim. */
    assert(solar_os_input_keyboard_captured_by(&right));
    assert(solar_os_input_write_key(source, 4, 4, 'a', 0, SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    foreground_poll_keyboard(&right);
    assert(right_keys.count == 1 && right_keys.keys[0].key == 'a');
    assert(solar_os_input_write_key(source, 5, 5, 'b', 0, SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);

    session_store_input_focus("left");
    assert(!solar_os_input_keyboard_captured_by(&right));
    /* Typing reaches the other pane before the old app's next tick. */
    assert(solar_os_input_write_key(source, 6, 6, 'c', 0, SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    solar_os_input_key_event_t desktop[4];
    assert(solar_os_input_read_events(desktop, 4) == 1 && desktop[0].key == 'c');
    assert(!solar_os_input_capture_key_event(&desktop[0]));
    foreground_poll_keyboard(&right);
    assert(!right.keyboard_captured && right_keys.count == 0 && atomic_load(&right.input_reset));

    foreground_poll_keyboard(&left);
    assert(left.keyboard_captured && solar_os_input_keyboard_captured_by(&left));
    foreground_poll_keyboard(&right);
    assert(solar_os_input_keyboard_captured_by(&left));
    /* Both translated Ctrl arrows and AltGr escape capture for focus switching. */
    for (unsigned alt = 0; alt < 2; alt++) {
        const uint8_t mods = SOLAR_OS_INPUT_MOD_LEFT_CTRL |
            (alt ? SOLAR_OS_INPUT_MOD_RIGHT_ALT : SOLAR_OS_INPUT_MOD_LEFT_ALT);
        assert(solar_os_input_write_key(source, 0x4f, 0x4f, SOLAR_OS_KEY_CTRL_RIGHT,
            mods, SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
        assert(solar_os_input_write_key(source, 0x4f, 0x4f, SOLAR_OS_KEY_CTRL_RIGHT,
            0, SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK);
        assert(solar_os_input_read_events(desktop, 4) == 2);
        assert(!solar_os_input_capture_key_event(&desktop[0]));
        assert(!solar_os_input_capture_key_event(&desktop[1]));
    }
    session_store_input_focus("right");
    foreground_poll_keyboard(&right); /* The new pane may tick before the old one. */
    assert(solar_os_input_keyboard_captured_by(&right));
    foreground_poll_keyboard(&left);
    assert(!left.keyboard_captured && atomic_load(&left.input_reset));
    assert(solar_os_input_keyboard_captured_by(&right));
    solar_os_input_release_keyboard(&right);

    int script;
    assert(solar_os_input_capture_keyboard("keyboard", &script) == ESP_OK);
    session_store_input_focus("left");
    assert(solar_os_input_keyboard_captured_by(&script)); /* Explicit raw script lease. */
    solar_os_input_release_keyboard(&script);
    assert(solar_os_sessions_focus_display(NULL) == ESP_ERR_INVALID_ARG);
    assert(solar_os_sessions_focus_display("") == ESP_ERR_INVALID_ARG);
    assert(solar_os_sessions_focus_display("missing") == ESP_ERR_NOT_FOUND);
    solar_os_input_source_close(source);
    puts("Native keyboard follows display focus, resets held keys, and preserves desktop shortcuts");
}
