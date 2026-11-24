/* Exercise native virtual input through the production input service. */
#define main input_fixture_tests
#include "input_test.c"
#undef main
#include "solar_os_qemu_input_codec.h"

static solar_os_qemu_input_state_t state = {.width = 800, .height = 600};

static void drain(void)
{
    solar_os_input_key_event_t keys[128];
    solar_os_input_pointer_event_t pointers[128];
    solar_os_input_axis_event_t axes[128];
    while (solar_os_input_read_events(keys, 128)) {}
    while (solar_os_input_read_pointer_events(pointers, 128)) {}
    while (solar_os_input_read_axis_events(axes, 128)) {}
}

static void key(uint16_t usage, uint8_t mods, bool down)
{
    const solar_os_qemu_input_event_t event = {
        .type = SOLAR_OS_QEMU_INPUT_KEY, .a = usage, .b = mods, .c = down,
    };
    assert(solar_os_qemu_input_process(&state, &event) == ESP_OK);
}

static void test_physical_keys_and_layout(void)
{
    key(0xe1, SOLAR_OS_INPUT_MOD_LEFT_SHIFT, true);
    key(0x04, SOLAR_OS_INPUT_MOD_LEFT_SHIFT, true);
    key(0x04, SOLAR_OS_INPUT_MOD_LEFT_SHIFT, true); /* host repeat is suppressed */
    key(0x04, SOLAR_OS_INPUT_MOD_LEFT_SHIFT, false);
    key(0xe1, 0, false);
    solar_os_input_key_event_t events[16];
    assert(solar_os_input_read_events(events, 16) == 4);
    assert(events[0].physical_key == 0xe1 && events[0].usage == 0xe1);
    assert(events[1].physical_key == 0x04 && events[1].usage == 0x04);
    assert(events[1].key == 'A' && events[1].modifiers == SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    assert(events[2].action == SOLAR_OS_INPUT_KEY_RELEASE && events[2].key == 'A');
    assert(events[3].action == SOLAR_OS_INPUT_KEY_RELEASE && events[3].modifiers == 0);

    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE) == ESP_OK);
    key(0xe6, SOLAR_OS_INPUT_MOD_RIGHT_ALT, true);
    key(0x08, SOLAR_OS_INPUT_MOD_RIGHT_ALT, true);
    assert(solar_os_input_read_events(events, 16) == 2);
    assert(events[1].usage == 0x08 && events[1].key != SOLAR_OS_KEY_APP_EXIT);
    solar_os_input_text_t text = solar_os_input_resolve_layout_key(&events[1]);
    assert(text.codepoint == 0x20ac); /* AltGr+E produces text through the layout service. */
    solar_os_qemu_input_release_all(&state);
    drain();
    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US) == ESP_OK);
}

static void test_touch_mouse_and_wheel(void)
{
    solar_os_qemu_input_event_t event = {.type = SOLAR_OS_QEMU_INPUT_TOUCH,
        .a = 32767, .b = 32767, .c = SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY};
    assert(solar_os_qemu_input_process(&state, &event) == ESP_OK);
    event.c = 0;
    assert(solar_os_qemu_input_process(&state, &event) == ESP_OK);
    solar_os_input_pointer_event_t pointers[8];
    assert(solar_os_input_read_pointer_events(pointers, 8) == 2);
    assert(pointers[0].source == state.touch && pointers[0].mode == SOLAR_OS_INPUT_POINTER_ABSOLUTE);
    assert(pointers[0].x == 799 && pointers[0].y == 599);
    assert(pointers[0].action == SOLAR_OS_INPUT_POINTER_PRESS);
    assert(pointers[1].action == SOLAR_OS_INPUT_POINTER_RELEASE && !pointers[1].buttons);
    assert(!strcmp(pointers[0].target, "display0"));

    event = (solar_os_qemu_input_event_t){.type = SOLAR_OS_QEMU_INPUT_MOUSE,
        .a = 7, .b = (uint32_t)-9, .c = 3, .d = (uint32_t)-2};
    assert(solar_os_qemu_input_process(&state, &event) == ESP_OK);
    assert(solar_os_input_read_pointer_events(pointers, 8) == 1);
    assert(pointers[0].source == state.mouse && pointers[0].mode == SOLAR_OS_INPUT_POINTER_RELATIVE);
    assert(pointers[0].delta_x == 7 && pointers[0].delta_y == -9 && pointers[0].buttons == 3);
    solar_os_input_axis_event_t axis;
    assert(solar_os_input_read_axis_events(&axis, 1) == 1);
    assert(axis.source == state.mouse && axis.axis == SOLAR_OS_INPUT_AXIS_Z && axis.delta == -2);
    solar_os_input_key_event_t keys[8];
    assert(solar_os_input_read_events(keys, 8) == 0); /* wheel is not a fake key */
}

static void test_release_all(void)
{
    key(0x04, 0, true);
    key(0x05, 0, true);
    drain();
    const solar_os_qemu_input_event_t reset = {.type = SOLAR_OS_QEMU_INPUT_RELEASE_ALL};
    assert(solar_os_qemu_input_process(&state, &reset) == ESP_OK);
    solar_os_input_key_event_t keys[8];
    assert(solar_os_input_read_events(keys, 8) == 2);
    for (unsigned i = 0; i < 2; i++) assert(keys[i].action == SOLAR_OS_INPUT_KEY_RELEASE);
    solar_os_input_pointer_event_t pointer[4];
    assert(solar_os_input_read_pointer_events(pointer, 4) == 1);
    assert(pointer[0].source == state.mouse && !pointer[0].buttons);
    assert(pointer[0].action == SOLAR_OS_INPUT_POINTER_RELEASE);
    assert(!state.pressed[0x04] && !state.pressed[0x05] && !state.mouse_buttons);
}

static void test_native_display_pixel_coordinates(void)
{
    const uint16_t sizes[][2] = {{300, 400}, {480, 320}};
    for (size_t profile = 0; profile < sizeof(sizes) / sizeof(sizes[0]); profile++) {
        state.width = sizes[profile][0];
        state.height = sizes[profile][1];
        for (unsigned axis = 0; axis < 2; axis++) {
            unsigned length = sizes[profile][axis];
            for (unsigned pixel = 0; pixel < length; pixel++) {
                solar_os_qemu_input_event_t event = {.type = SOLAR_OS_QEMU_INPUT_TOUCH,
                    .c = SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY};
                /* QEMU maps the frontend's logical pixels into 0..32767. */
                unsigned normalized = pixel * SOLAR_OS_QEMU_INPUT_AXIS_MAX / (length - 1);
                if (axis) event.b = normalized;
                else event.a = normalized;
                assert(solar_os_qemu_input_process(&state, &event) == ESP_OK);
                solar_os_input_pointer_event_t pointer;
                assert(solar_os_input_read_pointer_events(&pointer, 1) == 1);
                assert(pointer.x == (axis ? 0 : (int)pixel));
                assert(pointer.y == (axis ? (int)pixel : 0));
            }
        }
        assert(solar_os_qemu_input_release_all(&state) == ESP_OK);
        drain();
    }
    state.width = 800;
    state.height = 600;
}

static void test_pressure_retries_caps_lock_once(void)
{
    drain();
    solar_os_input_source_t filler;
    assert(solar_os_input_keyboard_source_open("qemu-filler", true, &filler) == ESP_OK);
    while (solar_os_input_write_char(filler, 'x') == ESP_OK) {}
    const solar_os_qemu_input_event_t caps = {.type = SOLAR_OS_QEMU_INPUT_KEY, .a = 0x39, .c = 1};
    assert(solar_os_qemu_input_process(&state, &caps) == ESP_ERR_NO_MEM);
    assert(!state.caps_lock && !state.pressed[0x39]);
    drain();
    assert(solar_os_qemu_input_process(&state, &caps) == ESP_OK);
    assert(state.caps_lock && state.pressed[0x39]);
    key(0x39, 0, false);
    solar_os_input_source_close(filler);
    drain();
}

static void test_focus_release_survives_a_full_key_queue(void)
{
    key(0x04, 0, true);
    drain();
    solar_os_input_source_t filler;
    assert(solar_os_input_keyboard_source_open("focus-filler", true, &filler) == ESP_OK);
    while (solar_os_input_write_char(filler, 'x') == ESP_OK) {}
    assert(solar_os_qemu_input_release_all(&state) == ESP_OK);
    solar_os_input_key_event_t events[128];
    const size_t count = solar_os_input_read_events(events, 128);
    bool released = false;
    for (size_t i = 0; i < count; i++) {
        released |= events[i].source == state.keyboard && events[i].usage == 0x04 &&
            events[i].action == SOLAR_OS_INPUT_KEY_RELEASE;
    }
    assert(released);
    solar_os_input_source_close(filler);
    drain();
}

static void test_focus_retries_a_full_pointer_queue(void)
{
    const solar_os_qemu_input_event_t press = {.type = SOLAR_OS_QEMU_INPUT_MOUSE, .c = 1};
    assert(solar_os_qemu_input_process(&state, &press) == ESP_OK);
    drain();
    solar_os_input_source_t filler;
    assert(solar_os_input_mouse_source_open("pointer-filler", &filler) == ESP_OK);
    const solar_os_input_pointer_event_t move = {.mode = SOLAR_OS_INPUT_POINTER_RELATIVE, .delta_x = 1};
    while (solar_os_input_write_pointer(filler, &move) == ESP_OK) {}
    assert(solar_os_qemu_input_release_all(&state) == ESP_ERR_NO_MEM);
    assert(state.mouse_buttons == 1);
    drain();
    assert(solar_os_qemu_input_release_all(&state) == ESP_OK);
    solar_os_input_pointer_event_t release;
    assert(solar_os_input_read_pointer_events(&release, 1) == 1);
    assert(release.source == state.mouse && release.action == SOLAR_OS_INPUT_POINTER_RELEASE);
    assert(!release.buttons && !state.mouse_buttons);
    solar_os_input_source_close(filler);
    drain();
}

static void test_wheel_pressure_does_not_repeat_mouse_movement(void)
{
    solar_os_input_source_t fillers[4];
    for (unsigned i = 0; i < 4; i++) {
        char name[16];
        snprintf(name, sizeof(name), "wheel-fill-%u", i);
        assert(solar_os_input_joystick_source_open(name, &fillers[i]) == ESP_OK);
        for (unsigned axis = 0; axis < SOLAR_OS_INPUT_AXIS_COUNT; axis++) {
            const solar_os_input_axis_event_t value = {.axis = (solar_os_input_axis_t)axis, .value = 1};
            (void)solar_os_input_write_axis(fillers[i], &value);
        }
    }
    for (unsigned axis = 0; axis < SOLAR_OS_INPUT_AXIS_COUNT; axis++) {
        const solar_os_input_axis_event_t value = {.axis = (solar_os_input_axis_t)axis, .value = 1};
        assert(solar_os_input_write_axis(state.touch, &value) == ESP_OK);
    }
    for (unsigned axis = 0; axis < 2; axis++) {
        const solar_os_input_axis_event_t value = {.axis = (solar_os_input_axis_t)axis, .value = 1};
        assert(solar_os_input_write_axis(state.mouse, &value) == ESP_OK);
    }
    const solar_os_qemu_input_event_t event = {.type = SOLAR_OS_QEMU_INPUT_MOUSE, .a = 7, .d = 1};
    assert(solar_os_qemu_input_process(&state, &event) == ESP_ERR_NO_MEM);
    solar_os_input_pointer_event_t movement;
    assert(solar_os_input_read_pointer_events(&movement, 1) == 1 && movement.delta_x == 7);
    solar_os_input_axis_event_t axes[64];
    assert(solar_os_input_read_axis_events(axes, 64) > 0);
    assert(solar_os_qemu_input_process(&state, &event) == ESP_OK);
    assert(solar_os_input_read_pointer_events(&movement, 1) == 0);
    assert(solar_os_input_read_axis_events(axes, 64) == 1 && axes[0].delta == 1);
    for (unsigned i = 0; i < 4; i++) solar_os_input_source_close(fillers[i]);
    drain();
}

int main(void)
{
    assert(solar_os_input_init() == ESP_OK);
    assert(solar_os_input_keyboard_source_open("qemu-keyboard", true, &state.keyboard) == ESP_OK);
    const uint32_t pointer = SOLAR_OS_INPUT_CAP_POINTER_BUTTONS | SOLAR_OS_INPUT_CAP_AXIS_EVENTS | SOLAR_OS_INPUT_CAP_SCROLL;
    assert(solar_os_input_source_open_typed("qemu-touch", SOLAR_OS_INPUT_SOURCE_TOUCH,
        pointer | SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE, true, &state.touch) == ESP_OK);
    assert(solar_os_input_source_open_typed("qemu-mouse", SOLAR_OS_INPUT_SOURCE_MOUSE,
        pointer | SOLAR_OS_INPUT_CAP_POINTER_RELATIVE, true, &state.mouse) == ESP_OK);
    test_physical_keys_and_layout();
    test_touch_mouse_and_wheel();
    test_release_all();
    test_pressure_retries_caps_lock_once();
    test_focus_release_survives_a_full_key_queue();
    test_focus_retries_a_full_pointer_queue();
    test_wheel_pressure_does_not_repeat_mouse_movement();
    test_native_display_pixel_coordinates();
    solar_os_input_source_close(state.keyboard);
    solar_os_input_source_close(state.touch);
    solar_os_input_source_close(state.mouse);
    puts("QEMU native input tests: ok");
}
