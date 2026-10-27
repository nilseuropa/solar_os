/* Exercise the real service using its deterministic host clock and NVS fixtures. */
#define main input_service_tests
#include "input_test.c"
#undef main

static unsigned observed;
static solar_os_input_gesture_event_t last_observed;

static void touch_observer(const solar_os_input_gesture_event_t *event, void *context)
{
    (void)context;
    /* Observers can query source metadata while handling an event. */
    solar_os_input_source_info_t info;
    assert(solar_os_input_source_find("touch", &info));
    observed++;
    last_observed = *event;
}

static void pointer_at(solar_os_input_source_t source, uint32_t ms,
    solar_os_input_pointer_action_t action, int16_t x, int16_t y, uint8_t id)
{
    now_us = (int64_t)ms * 1000;
    const solar_os_input_pointer_sample_t sample = {
        .time_ms = ms,
        .event = {.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE, .action = action,
            .x = x, .y = y, .pointer_id = id, .target = "display0",
            .buttons = action == SOLAR_OS_INPUT_POINTER_RELEASE ? 0 : 1},
    };
    assert(solar_os_input_write_pointer_sample(source, &sample) == ESP_OK);
    solar_os_input_pointer_event_t event;
    assert(solar_os_input_read_pointer_events(&event, 1) == 1);
    assert(event.source == source && event.x == x && event.y == y && event.action == action);
}

static void poll_at(uint32_t ms)
{
    now_us = (int64_t)ms * 1000;
    solar_os_input_poll();
}

static solar_os_input_gesture_event_t expect_gesture(solar_os_input_source_t source,
    solar_os_input_gesture_t kind, solar_os_input_gesture_direction_t direction)
{
    solar_os_input_gesture_event_t event;
    assert(solar_os_input_read_gesture_events(&event, 1) == 1);
    assert(event.source == source && event.gesture == kind && event.direction == direction);
    assert(memcmp(&last_observed, &event, sizeof(event)) == 0);
    return event;
}

static void expect_none(void)
{
    solar_os_input_gesture_event_t event;
    assert(solar_os_input_read_gesture_events(&event, 1) == 0);
}

int main(void)
{
    assert(solar_os_input_init() == ESP_OK);
    solar_os_input_source_t source, other;
    /* Any absolute touch source receives recognition, including mixed inputs. */
    assert(solar_os_input_source_open_typed("touch", SOLAR_OS_INPUT_SOURCE_TOUCH,
        SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE | SOLAR_OS_INPUT_CAP_POINTER_BUTTONS |
        SOLAR_OS_INPUT_CAP_KEY_EVENTS, true, &source) == ESP_OK);
    assert(solar_os_input_touch_source_open("other", &other) == ESP_OK);
    solar_os_input_source_info_t info;
    assert(solar_os_input_source_find("touch", &info));
    assert(info.capabilities & SOLAR_OS_INPUT_CAP_GESTURE_EVENTS);
    assert(info.gesture_mask == (SOLAR_OS_INPUT_GESTURE_MASK(SOLAR_OS_INPUT_GESTURE_TAP) |
        SOLAR_OS_INPUT_GESTURE_MASK(SOLAR_OS_INPUT_GESTURE_DOUBLE_TAP) |
        SOLAR_OS_INPUT_GESTURE_MASK(SOLAR_OS_INPUT_GESTURE_HOLD) |
        SOLAR_OS_INPUT_GESTURE_MASK(SOLAR_OS_INPUT_GESTURE_SWIPE)));
    solar_os_input_gesture_t parsed;
    assert(solar_os_input_parse_gesture("swipe", &parsed) && parsed == SOLAR_OS_INPUT_GESTURE_SWIPE);
    assert(solar_os_input_gesture_observer_register(touch_observer, NULL) == ESP_OK);

    pointer_at(source, 100, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    pointer_at(source, 200, SOLAR_OS_INPUT_POINTER_RELEASE, 102, 101, 0);
    poll_at(500); expect_none();
    poll_at(501);
    expect_gesture(source, SOLAR_OS_INPUT_GESTURE_TAP, SOLAR_OS_INPUT_GESTURE_DIRECTION_NONE);
    poll_at(900); expect_none();

    pointer_at(source, 1000, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    pointer_at(source, 1100, SOLAR_OS_INPUT_POINTER_RELEASE, 100, 100, 0);
    pointer_at(source, 1350, SOLAR_OS_INPUT_POINTER_PRESS, 110, 110, 1);
    poll_at(1450); expect_none();
    pointer_at(source, 1500, SOLAR_OS_INPUT_POINTER_RELEASE, 110, 110, 1);
    assert(expect_gesture(source, SOLAR_OS_INPUT_GESTURE_DOUBLE_TAP,
        SOLAR_OS_INPUT_GESTURE_DIRECTION_NONE).flags & SOLAR_OS_INPUT_GESTURE_FLAG_DOUBLE);
    poll_at(1900); expect_none();

    pointer_at(source, 2000, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    poll_at(2599); expect_none();
    poll_at(2600);
    assert(expect_gesture(source, SOLAR_OS_INPUT_GESTURE_HOLD,
        SOLAR_OS_INPUT_GESTURE_DIRECTION_NONE).value == 600);
    poll_at(2800); expect_none();
    pointer_at(source, 2900, SOLAR_OS_INPUT_POINTER_RELEASE, 100, 100, 0);
    expect_none();

    const int16_t ends[][2] = {{160, 105}, {40, 105}, {105, 40}, {105, 160}};
    const solar_os_input_gesture_direction_t directions[] = {
        SOLAR_OS_INPUT_GESTURE_DIRECTION_EAST, SOLAR_OS_INPUT_GESTURE_DIRECTION_WEST,
        SOLAR_OS_INPUT_GESTURE_DIRECTION_NORTH, SOLAR_OS_INPUT_GESTURE_DIRECTION_SOUTH};
    for (unsigned i = 0; i < 4; i++) {
        const uint32_t start = 3000 + 1000 * i;
        pointer_at(source, start, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
        pointer_at(source, start + 100, SOLAR_OS_INPUT_POINTER_MOVE, ends[i][0], ends[i][1], 0);
        pointer_at(source, start + 200, SOLAR_OS_INPUT_POINTER_RELEASE, ends[i][0], ends[i][1], 0);
        assert(expect_gesture(source, SOLAR_OS_INPUT_GESTURE_SWIPE, directions[i]).value == 60);
    }
    /* Movement cancels tap/hold even when the finger returns to its origin. */
    pointer_at(source, 7000, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    pointer_at(source, 7100, SOLAR_OS_INPUT_POINTER_MOVE, 120, 100, 0);
    poll_at(7700); expect_none();
    pointer_at(source, 7800, SOLAR_OS_INPUT_POINTER_RELEASE, 100, 100, 0);
    poll_at(8200); expect_none();
    /* A diagonal drag and a slow drag are not cardinal swipes. */
    pointer_at(source, 8300, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    pointer_at(source, 8400, SOLAR_OS_INPUT_POINTER_RELEASE, 160, 160, 0); expect_none();
    pointer_at(source, 8500, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    pointer_at(source, 8600, SOLAR_OS_INPUT_POINTER_MOVE, 150, 100, 0);
    pointer_at(source, 9600, SOLAR_OS_INPUT_POINTER_RELEASE, 200, 100, 0); expect_none();

    /* State and double-tap matching are independent for each source. */
    pointer_at(source, 10000, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    pointer_at(source, 10100, SOLAR_OS_INPUT_POINTER_RELEASE, 100, 100, 0);
    pointer_at(other, 10200, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    pointer_at(other, 10300, SOLAR_OS_INPUT_POINTER_RELEASE, 100, 100, 0);
    poll_at(10401);
    expect_gesture(source, SOLAR_OS_INPUT_GESTURE_TAP, SOLAR_OS_INPUT_GESTURE_DIRECTION_NONE);
    poll_at(10601);
    expect_gesture(other, SOLAR_OS_INPUT_GESTURE_TAP, SOLAR_OS_INPUT_GESTURE_DIRECTION_NONE);

    /* Contact changes and disconnects cancel incomplete recognition. */
    pointer_at(source, 11000, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    pointer_at(source, 11100, SOLAR_OS_INPUT_POINTER_RELEASE, 100, 100, 1);
    poll_at(12000); expect_none();
    pointer_at(source, 12100, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    assert(solar_os_input_source_set_ready(source, false) == ESP_OK);
    poll_at(13000); expect_none();
    assert(solar_os_input_source_set_ready(source, true) == ESP_OK);
    pointer_at(source, 13100, SOLAR_OS_INPUT_POINTER_RELEASE, 100, 100, 0); expect_none();

    /* An exclusive filter discards pending taps and all handwriting strokes. */
    pointer_at(source, 14000, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    pointer_at(source, 14100, SOLAR_OS_INPUT_POINTER_RELEASE, 100, 100, 0);
    assert(solar_os_input_pointer_filter_register(consume_pointer, &pointer_filter_calls) == ESP_OK);
    pointer_at(source, 14200, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    pointer_at(source, 14300, SOLAR_OS_INPUT_POINTER_RELEASE, 200, 100, 0);
    solar_os_input_pointer_filter_unregister(consume_pointer, &pointer_filter_calls);
    poll_at(15000); expect_none();

    /* Reuse of a source slot cannot resurrect a pending tap. */
    pointer_at(other, 16000, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    pointer_at(other, 16100, SOLAR_OS_INPUT_POINTER_RELEASE, 100, 100, 0);
    solar_os_input_source_close(other);
    assert(solar_os_input_touch_source_open("other", &other) == ESP_OK);
    poll_at(17000); expect_none();
    /* Unsigned time arithmetic works across the millisecond clock wrap. */
    pointer_at(source, UINT32_MAX - 100, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    pointer_at(source, 10, SOLAR_OS_INPUT_POINTER_RELEASE, 100, 100, 0);
    poll_at(311);
    expect_gesture(source, SOLAR_OS_INPUT_GESTURE_TAP, SOLAR_OS_INPUT_GESTURE_DIRECTION_NONE);
    assert(observed == 10);

    /* Distant taps remain separate, and a second contact may become a hold. */
    pointer_at(source, 20000, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    pointer_at(source, 20100, SOLAR_OS_INPUT_POINTER_RELEASE, 100, 100, 0);
    pointer_at(source, 20200, SOLAR_OS_INPUT_POINTER_PRESS, 200, 100, 0);
    expect_gesture(source, SOLAR_OS_INPUT_GESTURE_TAP, SOLAR_OS_INPUT_GESTURE_DIRECTION_NONE);
    pointer_at(source, 20300, SOLAR_OS_INPUT_POINTER_RELEASE, 200, 100, 0);
    poll_at(20601);
    expect_gesture(source, SOLAR_OS_INPUT_GESTURE_TAP, SOLAR_OS_INPUT_GESTURE_DIRECTION_NONE);
    pointer_at(source, 21000, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    pointer_at(source, 21100, SOLAR_OS_INPUT_POINTER_RELEASE, 100, 100, 0);
    pointer_at(source, 21200, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    poll_at(21800); /* Both reports can become due in the same poll. */
    solar_os_input_gesture_event_t batch[2];
    assert(solar_os_input_read_gesture_events(batch, 2) == 2);
    assert(batch[0].gesture == SOLAR_OS_INPUT_GESTURE_TAP);
    assert(batch[1].gesture == SOLAR_OS_INPUT_GESTURE_HOLD);
    pointer_at(source, 21900, SOLAR_OS_INPUT_POINTER_RELEASE, 100, 100, 0); expect_none();

    /* Captured strokes stay exclusive, including a release after capture ends. */
    static const char owner;
    solar_os_input_source_t captured;
    assert(solar_os_input_capture_pointer("touch", &owner, &captured) == ESP_OK);
    assert(captured == source);
    now_us = 22000000;
    solar_os_input_pointer_event_t event = {.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
        .action = SOLAR_OS_INPUT_POINTER_PRESS, .buttons = 1, .x = 100, .y = 100};
    assert(solar_os_input_write_pointer(source, &event) == ESP_OK);
    poll_at(22700); expect_none();
    solar_os_input_pointer_sample_t sample;
    bool reset;
    assert(solar_os_input_read_captured_pointer(source, &owner, &sample, &reset) == ESP_OK);
    assert(!reset && sample.event.action == SOLAR_OS_INPUT_POINTER_PRESS);
    solar_os_input_release_pointer(source, &owner);
    now_us = 22800000;
    event.action = SOLAR_OS_INPUT_POINTER_RELEASE; event.buttons = 0;
    assert(solar_os_input_write_pointer(source, &event) == ESP_OK);
    poll_at(23200); expect_none();

    /* Recognition uses calibrated pixels rather than raw controller units. */
    const solar_os_input_pointer_calibration_t calibration = {
        .min_x = 0, .max_x = 200, .min_y = 0, .max_y = 200,
        .width = 401, .height = 201,
    };
    assert(solar_os_input_pointer_calibration_set(source, &calibration) == ESP_OK);
    now_us = 24000000;
    event.action = SOLAR_OS_INPUT_POINTER_PRESS; event.buttons = 1;
    event.x = 100;
    assert(solar_os_input_write_pointer(source, &event) == ESP_OK);
    now_us = 24100000;
    event.action = SOLAR_OS_INPUT_POINTER_RELEASE; event.buttons = 0; event.x = 120;
    assert(solar_os_input_write_pointer(source, &event) == ESP_OK);
    assert(expect_gesture(source, SOLAR_OS_INPUT_GESTURE_SWIPE,
        SOLAR_OS_INPUT_GESTURE_DIRECTION_EAST).value == 40);
    solar_os_input_pointer_event_t pointers[32];
    assert(solar_os_input_read_pointer_events(pointers, 32) == 2);
    assert(solar_os_input_pointer_calibration_reset(source) == ESP_OK);

    /* A full gesture queue must not reject a raw release or notify observers. */
    const solar_os_input_gesture_event_t filler = {.gesture = SOLAR_OS_INPUT_GESTURE_TAP};
    for (unsigned i = 0; i < 32; i++)
        assert(solar_os_input_write_gesture(source, &filler) == ESP_OK);
    unsigned before = observed;
    pointer_at(source, 25000, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    pointer_at(source, 25100, SOLAR_OS_INPUT_POINTER_RELEASE, 160, 100, 0);
    assert(observed == before);
    solar_os_input_gesture_event_t gestures[32];
    assert(solar_os_input_read_gesture_events(gestures, 32) == 32);
    poll_at(26000); expect_none();

    /* A dropped pointer sample cancels its stroke rather than making a tap. */
    event.action = SOLAR_OS_INPUT_POINTER_PRESS; event.buttons = 1; event.x = 100;
    now_us = 27000000;
    assert(solar_os_input_write_pointer(source, &event) == ESP_OK);
    event.action = SOLAR_OS_INPUT_POINTER_MOVE;
    for (unsigned i = 0; i < 31; i++)
        assert(solar_os_input_write_pointer(source, &event) == ESP_OK);
    now_us = 27100000; event.x = 160;
    assert(solar_os_input_write_pointer(source, &event) == ESP_ERR_NO_MEM);
    assert(solar_os_input_read_pointer_events(pointers, 32) == 32);
    pointer_at(source, 27200, SOLAR_OS_INPUT_POINTER_RELEASE, 100, 100, 0);
    poll_at(28000); expect_none();

    /* One runtime poll advances every source without producer-specific hooks. */
    pointer_at(source, 29000, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    pointer_at(other, 29000, SOLAR_OS_INPUT_POINTER_PRESS, 100, 100, 0);
    pointer_at(source, 29100, SOLAR_OS_INPUT_POINTER_RELEASE, 100, 100, 0);
    pointer_at(other, 29100, SOLAR_OS_INPUT_POINTER_RELEASE, 100, 100, 0);
    before = observed;
    poll_at(29401);
    assert(observed == before + 2);
    assert(solar_os_input_read_gesture_events(batch, 2) == 2);
    assert(batch[0].gesture == SOLAR_OS_INPUT_GESTURE_TAP && batch[0].source == source);
    assert(batch[1].gesture == SOLAR_OS_INPUT_GESTURE_TAP && batch[1].source == other);
    solar_os_input_source_close(source);
    solar_os_input_source_close(other);
    puts("touch gesture tests: ok");
    return 0;
}
