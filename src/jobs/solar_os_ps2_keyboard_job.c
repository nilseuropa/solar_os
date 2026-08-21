#include "solar_os_ps2_keyboard_job.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ps2_bus.h"
#include "solar_os_buses.h"
#include "solar_os_input.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_ps2_keyboard.h"

#define PS2_KEYBOARD_TICK_MS 2U
#define PS2_KEYBOARD_USAGE_BITMAP_SIZE 32U

typedef struct {
    bool running;
    bool leased;
    bool caps_lock;
    uint8_t modifiers;
    uint8_t held[PS2_KEYBOARD_USAGE_BITMAP_SIZE];
    uint32_t transitions;
    uint32_t unsupported;
    uint32_t dropped;
    char bus_name[SOLAR_OS_BUS_NAME_MAX];
    char owner[SOLAR_OS_JOB_OWNER_MAX];
    solar_os_input_source_t input_source;
    solar_os_ps2_bus_t bus;
    solar_os_ps2_keyboard_decoder_t decoder;
} ps2_keyboard_state_t;

static const char *TAG = "solar_os_ps2_keyboard";
static ps2_keyboard_state_t ps2_keyboard;

static bool ps2_usage_held(uint16_t usage)
{
    return usage < PS2_KEYBOARD_USAGE_BITMAP_SIZE * 8U &&
        (ps2_keyboard.held[usage / 8U] & (uint8_t)(1U << (usage % 8U))) != 0;
}

static void ps2_set_usage_held(uint16_t usage, bool held)
{
    if (usage >= PS2_KEYBOARD_USAGE_BITMAP_SIZE * 8U) {
        return;
    }
    const uint8_t mask = (uint8_t)(1U << (usage % 8U));
    if (held) {
        ps2_keyboard.held[usage / 8U] |= mask;
    } else {
        ps2_keyboard.held[usage / 8U] &= (uint8_t)~mask;
    }
}

static uint8_t ps2_modifier_bit(uint16_t usage)
{
    return usage >= 0xe0U && usage <= 0xe7U
        ? (uint8_t)(1U << (usage - 0xe0U))
        : 0;
}

static void ps2_keyboard_cleanup(void)
{
    solar_os_ps2_bus_stop(&ps2_keyboard.bus);
    if (ps2_keyboard.input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(ps2_keyboard.input_source);
    }
    if (ps2_keyboard.leased) {
        (void)solar_os_bus_release(ps2_keyboard.bus_name,
                                   SOLAR_OS_BUS_PROTOCOL_PS2,
                                   ps2_keyboard.owner);
    }
    memset(&ps2_keyboard, 0, sizeof(ps2_keyboard));
}

static esp_err_t ps2_keyboard_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc != 2 || argv == NULL || argv[1] == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&ps2_keyboard, 0, sizeof(ps2_keyboard));
    strlcpy(ps2_keyboard.bus_name, argv[1], sizeof(ps2_keyboard.bus_name));
    esp_err_t err = solar_os_jobs_owner_name("ps2-keyboard",
                                             ps2_keyboard.owner,
                                             sizeof(ps2_keyboard.owner));
    if (err != ESP_OK) {
        ps2_keyboard_cleanup();
        return err;
    }

    solar_os_bus_info_t info;
    if (!solar_os_bus_find(ps2_keyboard.bus_name, SOLAR_OS_BUS_PROTOCOL_PS2, &info)) {
        ps2_keyboard_cleanup();
        return ESP_ERR_NOT_FOUND;
    }
    err = solar_os_bus_acquire(ps2_keyboard.bus_name,
                               SOLAR_OS_BUS_PROTOCOL_PS2,
                               ps2_keyboard.owner);
    if (err != ESP_OK) {
        ps2_keyboard_cleanup();
        return err;
    }
    ps2_keyboard.leased = true;

    err = solar_os_input_keyboard_source_open("ps2-keyboard",
                                              true,
                                              &ps2_keyboard.input_source);
    if (err == ESP_OK) {
        err = solar_os_ps2_bus_start(&ps2_keyboard.bus, &info.config.ps2);
    }
    if (err != ESP_OK) {
        ps2_keyboard_cleanup();
        return err;
    }

    solar_os_ps2_keyboard_decoder_reset(&ps2_keyboard.decoder);
    (void)solar_os_jobs_note_resource("ps2-keyboard",
                                      SOLAR_OS_JOB_RESOURCE_CUSTOM,
                                      ps2_keyboard.bus_name,
                                      "PS/2 keyboard");
    ps2_keyboard.running = true;
    SOLAR_OS_LOGI(TAG,
                  "listening on %s: clock=GPIO%d data=GPIO%d",
                  ps2_keyboard.bus_name,
                  info.config.ps2.clock_pin,
                  info.config.ps2.data_pin);
    return ESP_OK;
}

static void ps2_keyboard_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (ps2_keyboard.running) {
        solar_os_ps2_bus_stats_t stats;
        solar_os_ps2_bus_get_stats(&ps2_keyboard.bus, &stats);
        SOLAR_OS_LOGI(TAG,
                      "stopped: bytes=%u transitions=%u unsupported=%u dropped=%u frame_errors=%u overruns=%u",
                      (unsigned)stats.bytes,
                      (unsigned)ps2_keyboard.transitions,
                      (unsigned)ps2_keyboard.unsupported,
                      (unsigned)ps2_keyboard.dropped,
                      (unsigned)stats.frame_errors,
                      (unsigned)stats.overruns);
    }
    ps2_keyboard_cleanup();
}

static void ps2_keyboard_transition(const solar_os_ps2_key_transition_t *transition)
{
    if (transition == NULL || transition->usage >= PS2_KEYBOARD_USAGE_BITMAP_SIZE * 8U) {
        ps2_keyboard.unsupported++;
        return;
    }
    const bool held = ps2_usage_held(transition->usage);
    if (held == transition->pressed) {
        return;
    }
    ps2_set_usage_held(transition->usage, transition->pressed);

    const uint8_t modifier = ps2_modifier_bit(transition->usage);
    if (modifier != 0) {
        if (transition->pressed) {
            ps2_keyboard.modifiers |= modifier;
        } else {
            ps2_keyboard.modifiers &= (uint8_t)~modifier;
        }
    }
    if (transition->usage == 0x39U && transition->pressed) {
        ps2_keyboard.caps_lock = !ps2_keyboard.caps_lock;
    }

    const uint8_t key = solar_os_input_translate_hid_usage(transition->usage,
                                                           ps2_keyboard.modifiers,
                                                           ps2_keyboard.caps_lock);
    const esp_err_t err = solar_os_input_write_key(
        ps2_keyboard.input_source,
        transition->usage,
        transition->usage,
        key,
        ps2_keyboard.modifiers,
        transition->pressed ? SOLAR_OS_INPUT_KEY_PRESS : SOLAR_OS_INPUT_KEY_RELEASE);
    if (err == ESP_OK) {
        ps2_keyboard.transitions++;
    } else {
        ps2_keyboard.dropped++;
    }
}

static bool ps2_keyboard_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;
    if (!ps2_keyboard.running || event == NULL || event->type != SOLAR_OS_EVENT_TICK) {
        return false;
    }

    uint8_t bytes[SOLAR_OS_PS2_RX_BUFFER_SIZE];
    const size_t count = solar_os_ps2_bus_read(&ps2_keyboard.bus,
                                               bytes,
                                               sizeof(bytes));
    for (size_t i = 0; i < count; i++) {
        solar_os_ps2_key_transition_t transition;
        const solar_os_ps2_decode_result_t result = solar_os_ps2_keyboard_decode(
            &ps2_keyboard.decoder,
            bytes[i],
            &transition);
        if (result == SOLAR_OS_PS2_DECODE_KEY) {
            ps2_keyboard_transition(&transition);
        } else if (result == SOLAR_OS_PS2_DECODE_UNSUPPORTED) {
            ps2_keyboard.unsupported++;
        }
    }
    return false;
}

const solar_os_job_t solar_os_ps2_keyboard_job = {
    .name = "ps2-keyboard",
    .summary = "receive keyboard input from a named PS/2 bus",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = ps2_keyboard_start,
    .stop = ps2_keyboard_stop,
    .event = ps2_keyboard_event,
    .tick_interval_ms = PS2_KEYBOARD_TICK_MS,
    .tick_deadline_ms = 2U,
};
