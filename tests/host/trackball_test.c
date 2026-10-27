#define main input_service_tests
#include "input_test.c"
#undef main
#include "driver/gpio.h"
#include "solar_os_input_composition.h"
#include "solar_os_jobs.h"
#include "solar_os_pointer_arrows_job.h"
#include "solar_os_shell_io.h"
#include "solar_os_trackball.h"

static void (*handlers[64])(void *);
static void *handler_args[64];
static bool configured[64];
static gpio_int_type_t interrupts[64];
static int fail_pin = -1;
static unsigned stopped;
esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config->mode == GPIO_MODE_INPUT && config->pull_up_en == GPIO_PULLUP_ENABLE);
    assert(config->intr_type == GPIO_INTR_DISABLE);
    for (int pin = 0; pin < 64; pin++) {
        if (!(config->pin_bit_mask & (1ULL << pin)))
            continue;
        assert(!configured[pin]);
        configured[pin] = true;
    }
    return ESP_OK;
}
esp_err_t gpio_reset_pin(gpio_num_t pin)
{
    configured[pin] = false;
    return ESP_OK;
}
esp_err_t gpio_install_isr_service(int flags)
{
    (void)flags;
    return ESP_ERR_INVALID_STATE;
}
esp_err_t gpio_isr_handler_add(gpio_num_t pin, void (*handler)(void *), void *arg)
{
    if (pin == fail_pin)
        return ESP_FAIL;
    assert(!handlers[pin]);
    handlers[pin] = handler;
    handler_args[pin] = arg;
    return ESP_OK;
}
esp_err_t gpio_isr_handler_remove(gpio_num_t pin)
{
    handlers[pin] = NULL;
    return ESP_OK;
}
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type)
{
    interrupts[pin] = type;
    return ESP_OK;
}
esp_err_t solar_os_jobs_get_generation(const char *name, uint32_t *generation)
{
    assert(!strcmp(name, "pointer-arrows"));
    *generation = 1;
    return ESP_OK;
}
esp_err_t solar_os_jobs_mark_stopped(const char *name, uint32_t generation, esp_err_t error)
{
    assert(!strcmp(name, "pointer-arrows") && generation == 1 && error != ESP_OK);
    stopped++;
    return ESP_OK;
}
esp_err_t solar_os_jobs_note_resource(const char *name, solar_os_job_resource_type_t type,
                                      const char *resource, const char *detail)
{
    (void)name;
    (void)type;
    assert(resource && detail);
    return ESP_OK;
}
solar_os_shell_io_t *solar_os_context_shell_io(solar_os_context_t *ctx)
{
    (void)ctx;
    return NULL;
}
esp_err_t solar_os_shell_io_table_printf(solar_os_shell_io_t *io, const char *fmt, ...)
{
    (void)io;
    (void)fmt;
    return ESP_OK;
}

static solar_os_expansion_binding_t bindings[] = {
    {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "up", .value = 3},
    {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "down", .value = 15},
    {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "left", .value = 1},
    {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "right", .value = 2},
};
static void edge(int pin)
{
    now_us += 1000;
    assert(handlers[pin] && interrupts[pin] == GPIO_INTR_ANYEDGE);
    handlers[pin](handler_args[pin]);
}
static void tick(void)
{
    solar_os_trackball_poll();
    const solar_os_event_t event = {.type = SOLAR_OS_EVENT_TICK,
                                    .data.tick_ms = (uint32_t)(now_us / 1000)};
    (void)solar_os_pointer_arrows_job.event(NULL, &event);
}
static esp_err_t start(const char *source)
{
    char arg[40];
    snprintf(arg, sizeof(arg), "source=%s", source);
    char *args[] = {"pointer-arrows", arg, "min-vector=1"};
    return solar_os_pointer_arrows_job.start(NULL, 3, args);
}
static esp_err_t start_minimum(const char *value, bool option_first)
{
    char option[80];
    snprintf(option, sizeof(option), "min-vector=%s", value);
    char *args[] = {"pointer-arrows", option_first ? option : "source=trackball0",
                   option_first ? "source=trackball0" : option};
    return solar_os_pointer_arrows_job.start(NULL, 3, args);
}
static void expect_key(uint8_t key, uint8_t modifiers)
{
    solar_os_input_key_event_t events[4];
    assert(solar_os_input_read_events(events, 2) == 2);
    assert(events[0].key == key && events[0].modifiers == modifiers);
    assert(events[0].action == SOLAR_OS_INPUT_KEY_PRESS &&
           events[1].action == SOLAR_OS_INPUT_KEY_RELEASE);
    assert(events[0].physical_key != SOLAR_OS_INPUT_PHYSICAL_NONE);
}
static void expect_action_at(uint8_t key, solar_os_input_key_action_t action, uint8_t modifiers, int line)
{
    solar_os_input_key_event_t event;
    const size_t count = solar_os_input_read_events(&event, 1);
    if (count != 1 || event.key != key || event.action != action || event.modifiers != modifiers)
        fprintf(stderr, "line %d expected key=%u action=%u mods=%u; got count=%zu key=%u action=%u mods=%u\n",
                line, key, action, modifiers, count, count ? event.key : 0,
                count ? event.action : 0, count ? event.modifiers : 0);
    assert(count == 1);
    assert(event.key == key && event.action == action && event.modifiers == modifiers);
    assert(event.physical_key != SOLAR_OS_INPUT_PHYSICAL_NONE);
}
#define expect_action(key, action, modifiers) expect_action_at(key, action, modifiers, __LINE__)
static void no_key(void)
{
    solar_os_input_key_event_t event;
    assert(solar_os_input_read_events(&event, 1) == 0);
}
static void settle(void)
{
    now_us += 80000;
    tick();
}
static void roll_to_hold(int pin, uint8_t key)
{
    edge(pin); tick();
    now_us += 39000; edge(pin); tick();
    now_us += 39000; edge(pin); tick();
    expect_action(key, SOLAR_OS_INPUT_KEY_PRESS, 0);
}
static void write_sample(solar_os_input_source_t source, solar_os_input_pointer_event_t event)
{
    now_us += 1000;
    const solar_os_input_pointer_sample_t sample = {.event = event,
                                                    .time_ms = (uint32_t)(now_us / 1000)};
    assert(solar_os_input_write_pointer_sample(source, &sample) == ESP_OK);
}

int main(void)
{
    assert(solar_os_input_init() == ESP_OK);
    now_us = 1000000;
    fail_pin = 1;
    assert(solar_os_trackball_attach("trackball0", bindings, 4) == ESP_FAIL);
    for (int i = 0; i < 64; i++)
        assert(!handlers[i] && !configured[i]);
    fail_pin = -1;
    bindings[3].value = 1;
    assert(solar_os_trackball_attach("trackball0", bindings, 4) == ESP_ERR_INVALID_ARG);
    bindings[3].value = 2;
    assert(solar_os_trackball_attach("trackball0", bindings, 4) == ESP_OK);
    assert(solar_os_trackball_attach("trackball0", bindings, 4) == ESP_ERR_INVALID_STATE);
    solar_os_input_source_info_t track;
    assert(solar_os_input_source_find("trackball0", &track));
    assert(track.capabilities == SOLAR_OS_INPUT_CAP_POINTER_RELATIVE);
    edge(3);
    edge(2);
    solar_os_trackball_poll();
    solar_os_input_pointer_event_t pointer[8];
    assert(solar_os_input_read_pointer_events(pointer, 8) == 2);
    assert(pointer[0].delta_y == -1 && pointer[1].delta_x == 1);
    assert(start("missing") == ESP_ERR_NOT_FOUND);
    const char *invalid_minima[] = {"", "0", "-1", "+1", " 2", "2x", "65536",
                                    "999999999999999999999999999999"};
    for (size_t i = 0; i < sizeof(invalid_minima) / sizeof(invalid_minima[0]); i++) {
        assert(start_minimum(invalid_minima[i], false) == ESP_ERR_INVALID_ARG);
        assert(!solar_os_input_pointer_is_captured(track.source));
    }
    char *duplicate_source[] = {"pointer-arrows", "source=trackball0", "source=trackball0"};
    char *duplicate_minimum[] = {"pointer-arrows", "min-vector=2", "min-vector=3"};
    char *unknown_option[] = {"pointer-arrows", "source=trackball0", "sensitivity=2"};
    assert(solar_os_pointer_arrows_job.start(NULL, 3, duplicate_source) == ESP_ERR_INVALID_ARG);
    assert(solar_os_pointer_arrows_job.start(NULL, 3, duplicate_minimum) == ESP_ERR_INVALID_ARG);
    assert(solar_os_pointer_arrows_job.start(NULL, 3, unknown_option) == ESP_ERR_INVALID_ARG);
    assert(start("trackball0") == ESP_OK);
    static const int another_owner;
    solar_os_input_source_t claim;
    assert(solar_os_input_capture_pointer("trackball0", &another_owner, &claim) ==
           ESP_ERR_INVALID_STATE);
    assert(solar_os_input_pointer_is_captured(track.source));
    /* Existing global observers and another pointer are independent. */
    solar_os_input_source_t touch;
    assert(solar_os_input_touch_source_open("touch0", &touch) == ESP_OK);
    assert(solar_os_input_pointer_filter_register(consume_pointer, &pointer_filter_calls) ==
           ESP_OK);
    write_sample(touch, (solar_os_input_pointer_event_t){
                            .mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE, .x = 40, .y = 50});
    edge(3);
    edge(3);
    edge(2);
    tick();
    no_key();
    assert(solar_os_input_take_captured_activity());
    assert(!solar_os_input_take_captured_activity());
    assert(solar_os_input_read_pointer_events(pointer, 8) == 1 && pointer[0].source == touch);
    assert(solar_os_input_pointer_filter_event(&pointer[0]));
    settle();
    expect_key(SOLAR_OS_KEY_UP, 0);
    /* Evaluation uses fixed windows while rolling; repeat uses normal key settings. */
    assert(solar_os_input_set_repeat(10, 100) == ESP_OK);
    edge(2); tick(); no_key();
    now_us += 39000; edge(2); tick(); no_key();
    now_us += 39000; tick(); no_key();
    edge(2); tick();
    expect_action(SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_KEY_PRESS, 0);
    solar_os_input_key_event_t held[4];
    assert(solar_os_input_get_pressed(held, 4) == 1 && held[0].key == SOLAR_OS_KEY_RIGHT);
    now_us += 39000; edge(2); tick(); no_key();
    now_us += 39000; edge(2); tick(); no_key();
    now_us += 19000; edge(2); tick();
    expect_action(SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_KEY_REPEAT, 0);
    now_us += 39000; edge(2); tick(); no_key();
    now_us += 79000; tick();
    expect_action(SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_KEY_REPEAT, 0);
    now_us += 1000; tick();
    expect_action(SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_KEY_RELEASE, 0);
    assert(solar_os_input_get_pressed(held, 4) == 0);
    assert(solar_os_input_set_repeat(0, 100) == ESP_OK);

    /* The minimum measures net vector length, including partial windows. */
    solar_os_pointer_arrows_job.stop(NULL);
    assert(start_minimum("4", false) == ESP_OK);
    edge(2); edge(2); edge(2); tick(); settle(); no_key();
    edge(2); edge(2); edge(2); edge(2); tick(); settle();
    expect_key(SOLAR_OS_KEY_RIGHT, 0);
    solar_os_pointer_arrows_job.stop(NULL);
    assert(start_minimum("5", true) == ESP_OK);
    write_sample(track.source, (solar_os_input_pointer_event_t){
        .mode = SOLAR_OS_INPUT_POINTER_RELATIVE, .delta_x = 3, .delta_y = 3});
    tick(); settle(); no_key(); /* Two axes of 3 have length less than 5. */
    write_sample(track.source, (solar_os_input_pointer_event_t){
        .mode = SOLAR_OS_INPUT_POINTER_RELATIVE, .delta_x = 3, .delta_y = -4});
    tick(); settle(); expect_key(SOLAR_OS_KEY_UP, 0); /* Length exactly 5. */
    write_sample(track.source, (solar_os_input_pointer_event_t){
        .mode = SOLAR_OS_INPUT_POINTER_RELATIVE, .delta_x = -5});
    write_sample(track.source, (solar_os_input_pointer_event_t){
        .mode = SOLAR_OS_INPUT_POINTER_RELATIVE, .delta_x = 4});
    tick(); settle(); no_key(); /* Total travel does not defeat cancellation. */

    /* Weak movement releases a hold and cannot accumulate across windows. */
    write_sample(track.source, (solar_os_input_pointer_event_t){
        .mode = SOLAR_OS_INPUT_POINTER_RELATIVE, .delta_x = 5});
    tick();
    now_us += 39000; edge(2); tick();
    now_us += 39000; edge(2); tick();
    expect_action(SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_KEY_PRESS, 0);
    now_us += 39000; edge(2); tick();
    now_us += 39000; edge(2); tick();
    expect_action(SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_KEY_RELEASE, 0);
    for (int i = 0; i < 6; i++) {
        now_us += 39000; edge(2); tick(); no_key();
    }
    settle(); no_key();
    solar_os_pointer_arrows_job.stop(NULL);
    assert(start_minimum("65535", false) == ESP_OK);
    write_sample(track.source, (solar_os_input_pointer_event_t){
        .mode = SOLAR_OS_INPUT_POINTER_RELATIVE, .delta_x = INT16_MAX, .delta_y = INT16_MIN});
    tick(); settle(); no_key();
    for (int i = 0; i < 2; i++)
        write_sample(track.source, (solar_os_input_pointer_event_t){
            .mode = SOLAR_OS_INPUT_POINTER_RELATIVE, .delta_x = INT16_MAX, .delta_y = INT16_MIN});
    tick(); settle(); expect_key(SOLAR_OS_KEY_UP, 0);
    solar_os_pointer_arrows_job.stop(NULL);
    char *default_args[] = {"pointer-arrows", "source=trackball0"};
    assert(solar_os_pointer_arrows_job.start(NULL, 2, default_args) == ESP_OK);
    edge(2); tick(); settle(); no_key(); /* Omitting the option restores 2. */
    edge(2); edge(2); tick(); settle(); expect_key(SOLAR_OS_KEY_RIGHT, 0);
    solar_os_pointer_arrows_job.stop(NULL);
    assert(start("trackball0") == ESP_OK);

    /* A turn releases the previous key before pressing the new direction. */
    edge(2); tick();
    now_us += 39000; edge(2); tick();
    now_us += 39000; edge(2); tick();
    expect_action(SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_KEY_PRESS, 0);
    now_us += 39000; edge(1); tick(); no_key();
    edge(1); tick(); no_key();
    now_us += 38000; edge(1); tick();
    expect_action(SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_KEY_RELEASE, 0);
    expect_action(SOLAR_OS_KEY_LEFT, SOLAR_OS_INPUT_KEY_PRESS, 0);
    settle();
    expect_action(SOLAR_OS_KEY_LEFT, SOLAR_OS_INPUT_KEY_RELEASE, 0);

    /* Stopping during a hold releases it and cancels future repeats. */
    edge(3); tick();
    now_us += 39000; edge(3); tick();
    now_us += 39000; edge(3); tick();
    expect_action(SOLAR_OS_KEY_UP, SOLAR_OS_INPUT_KEY_PRESS, 0);
    solar_os_pointer_arrows_job.stop(NULL);
    expect_action(SOLAR_OS_KEY_UP, SOLAR_OS_INPUT_KEY_RELEASE, 0);
    assert(solar_os_input_get_pressed(held, 4) == 0);
    assert(start("trackball0") == ESP_OK);
    /* A capture overflow neutralizes an existing hold before discarding motion. */
    roll_to_hold(2, SOLAR_OS_KEY_RIGHT);
    for (unsigned i = 0; i < 40; i++)
        write_sample(track.source, (solar_os_input_pointer_event_t){
            .mode = SOLAR_OS_INPUT_POINTER_RELATIVE, .delta_x = 1});
    tick();
    expect_action(SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_KEY_RELEASE, 0);
    assert(solar_os_input_get_pressed(held, 4) == 0);
    settle(); no_key();
    /* Opposite edges cancel; equal diagonals retain the final moved axis. */
    edge(1);
    edge(2);
    tick();
    settle();
    no_key();
    edge(3);
    edge(2);
    tick();
    settle();
    expect_key(SOLAR_OS_KEY_RIGHT, 0);
    /* Two strokes remain distinct even if polling was delayed. */
    edge(3);
    now_us += 80000;
    edge(1);
    now_us += 80000;
    tick();
    expect_key(SOLAR_OS_KEY_UP, 0);
    expect_key(SOLAR_OS_KEY_LEFT, 0);
    no_key();
    solar_os_input_source_t keyboard;
    assert(solar_os_input_keyboard_source_open("modifiers", true, &keyboard) == ESP_OK);
    roll_to_hold(2, SOLAR_OS_KEY_RIGHT);
    solar_os_input_composition_set_modifiers(keyboard, SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    now_us += 39000; edge(2); tick(); no_key();
    now_us += 39000; edge(2); tick();
    expect_action(SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_KEY_RELEASE, SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    expect_action(SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_KEY_PRESS, SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    settle();
    expect_action(SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_KEY_RELEASE, SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    solar_os_input_composition_set_modifiers(keyboard, SOLAR_OS_INPUT_MOD_LEFT_ALT);
    edge(3);
    tick();
    solar_os_input_composition_set_modifiers(keyboard, 0);
    settle();
    expect_key(SOLAR_OS_KEY_PAGE_UP, 0);
    solar_os_input_composition_set_modifiers(keyboard, SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    edge(1);
    tick();
    solar_os_input_composition_set_modifiers(keyboard, 0);
    settle();
    expect_key(SOLAR_OS_KEY_LEFT, SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    /* Key-queue backpressure retains both strokes without duplicating taps. */
    for (unsigned i = 0; i < 32; i++)
        assert(solar_os_input_write_key_tap(keyboard, 0, 0, 'k', 0) == ESP_OK);
    edge(3);
    tick();
    settle();
    edge(1);
    tick();
    solar_os_input_key_event_t full[64];
    assert(solar_os_input_read_events(full, 64) == 64);
    for (unsigned i = 0; i < 64; i++)
        assert(full[i].key == 'k');
    tick();
    expect_key(SOLAR_OS_KEY_UP, 0);
    settle();
    expect_key(SOLAR_OS_KEY_LEFT, 0);
    no_key();
    /* Source queues and unpublished interrupt edges cannot cross a stop. */
    edge(3);
    solar_os_pointer_arrows_job.stop(NULL);
    assert(!solar_os_input_pointer_is_captured(track.source));
    settle();
    no_key();
    assert(solar_os_input_read_pointer_events(pointer, 8) == 0);
    edge(2);
    tick();
    assert(solar_os_input_read_pointer_events(pointer, 8) == 1 && pointer[0].delta_x == 1);
    edge(3);
    solar_os_trackball_poll();
    assert(start("trackball0") == ESP_OK);
    assert(solar_os_input_read_pointer_events(pointer, 8) == 0);
    edge(1);
    tick();
    no_key();
    solar_os_pointer_arrows_job.stop(NULL);
    settle();
    no_key();

    /* Capture is independent per source, and overflow has an explicit reset. */
    assert(solar_os_input_capture_pointer("trackball0", &another_owner, &claim) == ESP_OK);
    static const int touch_owner;
    solar_os_input_source_t touch_claim;
    assert(solar_os_input_capture_pointer("touch0", &touch_owner, &touch_claim) == ESP_OK);
    for (unsigned i = 0; i < 33; i++)
        write_sample(track.source, (solar_os_input_pointer_event_t){
                                       .mode = SOLAR_OS_INPUT_POINTER_RELATIVE, .delta_x = 1});
    solar_os_input_pointer_sample_t sample;
    bool reset = false;
    assert(solar_os_input_read_captured_pointer(claim, &another_owner, &sample, &reset) == ESP_OK &&
           reset);
    assert(solar_os_input_read_captured_pointer(claim, &another_owner, &sample, &reset) ==
               ESP_ERR_NOT_FOUND &&
           !reset);
    solar_os_input_release_pointer(claim, &touch_owner);
    assert(solar_os_input_pointer_is_captured(claim));
    solar_os_input_release_pointer(claim, &another_owner);
    solar_os_input_release_pointer(touch_claim, &touch_owner);
    fail_pointer_capture_allocation = true;
    assert(start("trackball0") == ESP_ERR_NO_MEM);
    fail_pointer_capture_allocation = false;
    assert(!solar_os_input_pointer_is_captured(track.source));
    solar_os_input_source_t fillers[5];
    for (unsigned i = 0; i < 5; i++) {
        char name[16];
        snprintf(name, sizeof(name), "fill%u", i);
        assert(solar_os_input_source_open(name, &fillers[i]) == ESP_OK);
    }
    assert(start("trackball0") == ESP_ERR_NO_MEM);
    assert(!solar_os_input_pointer_is_captured(track.source));
    for (unsigned i = 0; i < 5; i++)
        solar_os_input_source_close(fillers[i]);
    assert(start("trackball0") == ESP_OK);
    for (unsigned i = 0; i < 40; i++)
        write_sample(track.source, (solar_os_input_pointer_event_t){
                                       .mode = SOLAR_OS_INPUT_POINTER_RELATIVE, .delta_x = 1});
    tick();
    settle();
    no_key(); /* Overflow discards the incomplete stroke. */
    edge(3);
    tick();
    settle();
    expect_key(SOLAR_OS_KEY_UP, 0);
    solar_os_pointer_arrows_job.stop(NULL);

    /* Absolute contact baselines do not jump or join across touches. */
    char *touch_args[] = {"pointer-arrows", "source=touch0", "min-vector=5"};
    assert(solar_os_pointer_arrows_job.start(NULL, 3, touch_args) == ESP_OK);
    for (int displacement = 3; displacement <= 4; displacement++) {
        write_sample(touch, (solar_os_input_pointer_event_t){
            .mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE, .action = SOLAR_OS_INPUT_POINTER_PRESS,
            .x = 100, .y = 100, .buttons = 1});
        tick();
        write_sample(touch, (solar_os_input_pointer_event_t){
            .mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE, .action = SOLAR_OS_INPUT_POINTER_RELEASE,
            .x = 103, .y = 100 + displacement});
        tick();
        if (displacement == 3) no_key();
        else expect_key(SOLAR_OS_KEY_DOWN, 0);
    }
    solar_os_pointer_arrows_job.stop(NULL);
    assert(start("touch0") == ESP_OK);
    write_sample(touch, (solar_os_input_pointer_event_t){.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
                                                         .action = SOLAR_OS_INPUT_POINTER_PRESS,
                                                         .x = 100,
                                                         .y = 100,
                                                         .buttons = 1});
    tick();
    write_sample(touch,
                 (solar_os_input_pointer_event_t){
                     .mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE, .x = 130, .y = 98, .buttons = 1});
    tick();
    write_sample(touch, (solar_os_input_pointer_event_t){.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
                                                         .action = SOLAR_OS_INPUT_POINTER_RELEASE,
                                                         .x = 130,
                                                         .y = 98});
    tick();
    settle();
    expect_key(SOLAR_OS_KEY_RIGHT, 0);
    /* Releasing another contact must not release the tracked contact's hold. */
    write_sample(touch, (solar_os_input_pointer_event_t){.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
        .action = SOLAR_OS_INPUT_POINTER_PRESS, .x = 100, .y = 100, .buttons = 1});
    tick();
    write_sample(touch, (solar_os_input_pointer_event_t){.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
        .x = 110, .y = 100, .buttons = 1});
    tick();
    now_us += 39000;
    write_sample(touch, (solar_os_input_pointer_event_t){.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
        .x = 120, .y = 100, .buttons = 1});
    tick();
    now_us += 39000;
    write_sample(touch, (solar_os_input_pointer_event_t){.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
        .x = 130, .y = 100, .buttons = 1});
    tick();
    expect_action(SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_KEY_PRESS, 0);
    write_sample(touch, (solar_os_input_pointer_event_t){.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
        .action = SOLAR_OS_INPUT_POINTER_RELEASE, .pointer_id = 1});
    tick(); no_key();
    assert(solar_os_input_get_pressed(held, 4) == 1);
    write_sample(touch, (solar_os_input_pointer_event_t){.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
        .action = SOLAR_OS_INPUT_POINTER_RELEASE, .x = 130, .y = 100});
    tick();
    expect_action(SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_KEY_RELEASE, 0);
    write_sample(touch, (solar_os_input_pointer_event_t){.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
                                                         .action = SOLAR_OS_INPUT_POINTER_PRESS,
                                                         .x = 300,
                                                         .y = 200,
                                                         .buttons = 1});
    tick();
    write_sample(touch, (solar_os_input_pointer_event_t){.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
                                                         .action = SOLAR_OS_INPUT_POINTER_RELEASE,
                                                         .x = 300,
                                                         .y = 200});
    tick();
    settle();
    no_key();
    assert(solar_os_input_read_pointer_events(pointer, 8) == 0);
    write_sample(touch, (solar_os_input_pointer_event_t){.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
                                                         .action = SOLAR_OS_INPUT_POINTER_PRESS,
                                                         .x = 10,
                                                         .y = 20,
                                                         .buttons = 1});
    tick();
    solar_os_pointer_arrows_job.stop(NULL);
    write_sample(touch,
                 (solar_os_input_pointer_event_t){
                     .mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE, .x = 50, .y = 20, .buttons = 1});
    write_sample(touch, (solar_os_input_pointer_event_t){.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
                                                         .action = SOLAR_OS_INPUT_POINTER_RELEASE,
                                                         .x = 50,
                                                         .y = 20});
    assert(solar_os_input_read_pointer_events(pointer, 8) == 0);
    write_sample(touch, (solar_os_input_pointer_event_t){.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
                                                         .action = SOLAR_OS_INPUT_POINTER_PRESS,
                                                         .x = 10,
                                                         .y = 20,
                                                         .buttons = 1});
    assert(start("touch0") == ESP_ERR_INVALID_STATE); /* Release before taking ownership. */
    write_sample(touch, (solar_os_input_pointer_event_t){.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
                                                         .action = SOLAR_OS_INPUT_POINTER_RELEASE,
                                                         .x = 10,
                                                         .y = 20});
    assert(solar_os_input_read_pointer_events(pointer, 8) == 2);

    /* Detach stops conversion, frees capture, and never claims a reused ID. */
    void (*late_irq)(void *) = handlers[3];
    void *late_arg = handler_args[3];
    assert(start("trackball0") == ESP_OK);
    roll_to_hold(3, SOLAR_OS_KEY_UP);
    assert(solar_os_trackball_detach("trackball0") == ESP_OK);
    assert(!solar_os_trackball_active());
    tick();
    expect_action(SOLAR_OS_KEY_UP, SOLAR_OS_INPUT_KEY_RELEASE, 0);
    settle();
    no_key();
    assert(stopped == 1);
    for (int i = 0; i < 64; i++)
        assert(!handlers[i] && !configured[i]);
    assert(solar_os_trackball_attach("trackball0", bindings, 4) == ESP_OK);
    late_irq(late_arg); /* A detached generation may not touch the new device. */
    edge(1);
    tick();
    assert(solar_os_input_read_pointer_events(pointer, 8) == 1 && pointer[0].delta_x == -1);
    /* Stroke timestamps and capture boundaries survive millisecond wrap. */
    now_us = ((int64_t)UINT32_MAX - 40) * 1000;
    assert(start("trackball0") == ESP_OK);
    roll_to_hold(2, SOLAR_OS_KEY_RIGHT);
    settle();
    expect_action(SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_KEY_RELEASE, 0);
    solar_os_pointer_arrows_job.stop(NULL);
    assert(start("trackball0") == ESP_OK);
    assert(solar_os_input_source_find("trackball0", &track));
    roll_to_hold(2, SOLAR_OS_KEY_RIGHT);
    assert(solar_os_input_source_set_ready(track.source, false) == ESP_OK);
    tick();
    expect_action(SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_KEY_RELEASE, 0);
    assert(stopped == 2 && !solar_os_input_pointer_is_captured(track.source));
    assert(solar_os_input_source_set_ready(track.source, true) == ESP_OK);
    assert(solar_os_trackball_detach("trackball0") == ESP_OK);
    solar_os_input_source_close(keyboard);
    solar_os_input_source_close(touch);
    puts("Trackball IRQ lifecycle, pointer capture, modifiers, strokes, touch and mode boundaries "
         "passed");
    return 0;
}
