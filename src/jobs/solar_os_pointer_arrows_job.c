#include "solar_os_pointer_arrows_job.h"

#include <limits.h>
#include <string.h>
#include "solar_os_input.h"
#include "solar_os_input_composition.h"
#include "solar_os_jobs.h"
#include "solar_os_keys.h"
#include "solar_os_shell_io.h"

#define POINTER_ARROWS_EVALUATE_MS 80U
#define POINTER_ARROWS_QUIET_MS 80U
#define POINTER_ARROWS_PHYSICAL_KEY 1U
typedef struct {
    solar_os_input_source_t source, keyboard;
    char name[SOLAR_OS_INPUT_SOURCE_NAME_MAX];
    uint32_t generation, last_ms, window_ms, emitted, resets;
    int32_t x, y;
    int16_t absolute_x, absolute_y;
    uint16_t min_vector;
    uint8_t pointer_id, modifiers;
    bool running, pending, moving, horizontal, contact, held_sample, skip_stroke, release_pending;
    solar_os_input_pointer_sample_t sample;
} pointer_arrows_t;
static pointer_arrows_t arrows;

static void clear_motion(void)
{
    arrows.x = arrows.y = 0;
    arrows.pending = false;
    arrows.moving = false;
    arrows.contact = false;
}

static bool release_key(void)
{
    return solar_os_input_set_key(arrows.keyboard, POINTER_ARROWS_PHYSICAL_KEY,
                                  SOLAR_OS_INPUT_USAGE_NONE, 0,
                                  solar_os_input_composition_modifiers()) == ESP_OK;
}

static bool evaluate_motion(void)
{
    if (!arrows.pending)
        return true;
    const uint64_t x = arrows.x < 0 ? -(int64_t)arrows.x : arrows.x;
    const uint64_t y = arrows.y < 0 ? -(int64_t)arrows.y : arrows.y;
    const uint64_t minimum = arrows.min_vector;
    if (x * x + y * y >= minimum * minimum) {
        const bool horizontal = x > y || (x == y && arrows.horizontal);
        const uint8_t raw = horizontal ? (arrows.x < 0 ? SOLAR_OS_KEY_LEFT : SOLAR_OS_KEY_RIGHT)
                                       : (arrows.y < 0 ? SOLAR_OS_KEY_UP : SOLAR_OS_KEY_DOWN);
        const uint8_t key = solar_os_input_composition_apply_with_modifiers(raw, arrows.modifiers);
        const uint16_t usage = raw == SOLAR_OS_KEY_UP ? 0x52U :
            raw == SOLAR_OS_KEY_DOWN ? 0x51U : raw == SOLAR_OS_KEY_LEFT ? 0x50U : 0x4fU;
        if (solar_os_input_set_key(arrows.keyboard, POINTER_ARROWS_PHYSICAL_KEY,
                                   usage, key, key == raw ? arrows.modifiers : 0U) != ESP_OK)
            return false;
        arrows.emitted++;
    } else if (!release_key()) return false;
    arrows.x = arrows.y = 0;
    arrows.pending = false;
    return true;
}

static bool finish_motion(void)
{
    if (!evaluate_motion() || !release_key()) return false;
    arrows.moving = false;
    return true;
}

static bool advance_motion(uint32_t time_ms)
{
    if (!arrows.moving) return true;
    if ((int32_t)(time_ms - arrows.window_ms) >= (int32_t)POINTER_ARROWS_EVALUATE_MS) {
        if (!evaluate_motion()) return false;
        arrows.window_ms += ((time_ms - arrows.window_ms) / POINTER_ARROWS_EVALUATE_MS) *
            POINTER_ARROWS_EVALUATE_MS;
    }
    return (int32_t)(time_ms - arrows.last_ms) < (int32_t)POINTER_ARROWS_QUIET_MS || finish_motion();
}

static int32_t add_delta(int32_t total, int32_t delta)
{
    const int64_t sum = (int64_t)total + delta;
    return sum > INT32_MAX ? INT32_MAX : sum < INT32_MIN ? INT32_MIN : (int32_t)sum;
}

static void pointer_arrows_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (arrows.source)
        solar_os_input_release_pointer(arrows.source, &arrows);
    if (arrows.keyboard) {
        (void)release_key();
        solar_os_input_source_close(arrows.keyboard);
    }
    arrows.running = false;
    arrows.source = arrows.keyboard = SOLAR_OS_INPUT_SOURCE_INVALID;
    arrows.held_sample = false;
    clear_motion();
}

static esp_err_t pointer_arrows_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (!argv || argc < 2 || argc > 3) return ESP_ERR_INVALID_ARG;
    const char *source = NULL;
    uint32_t minimum = 2U;
    bool have_minimum = false;
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) return ESP_ERR_INVALID_ARG;
        if (!strncmp(argv[i], "source=", 7)) {
            if (source || !argv[i][7] || strlen(argv[i] + 7) >= sizeof(arrows.name))
                return ESP_ERR_INVALID_ARG;
            source = argv[i] + 7;
        } else if (!strncmp(argv[i], "min-vector=", 11)) {
            if (have_minimum || !argv[i][11]) return ESP_ERR_INVALID_ARG;
            have_minimum = true;
            minimum = 0;
            for (const char *p = argv[i] + 11; *p; p++) {
                if (*p < '0' || *p > '9') return ESP_ERR_INVALID_ARG;
                minimum = minimum * 10U + (uint32_t)(*p - '0');
                if (minimum > UINT16_MAX) return ESP_ERR_INVALID_ARG;
            }
            if (!minimum) return ESP_ERR_INVALID_ARG;
        } else return ESP_ERR_INVALID_ARG;
    }
    if (!source) return ESP_ERR_INVALID_ARG;
    memset(&arrows, 0, sizeof(arrows));
    arrows.min_vector = (uint16_t)minimum;
    strlcpy(arrows.name, source, sizeof(arrows.name));
    esp_err_t err = solar_os_input_capture_pointer(arrows.name, &arrows, &arrows.source);
    if (err == ESP_OK)
        err = solar_os_input_keyboard_source_open("pointer-arrows", true, &arrows.keyboard);
    if (err == ESP_OK)
        err = solar_os_jobs_get_generation("pointer-arrows", &arrows.generation);
    if (err != ESP_OK) {
        pointer_arrows_stop(ctx);
        return err;
    }
    arrows.running = true;
    (void)solar_os_jobs_note_resource("pointer-arrows", SOLAR_OS_JOB_RESOURCE_CUSTOM, arrows.name,
                                      "exclusive pointer capture");
    return ESP_OK;
}

static bool pointer_arrows_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (!arrows.running || event->type != SOLAR_OS_EVENT_TICK)
        return false;
    bool drained = false;
    for (size_t n = 0; n < 32U; n++) {
        if (arrows.release_pending) {
            if (!release_key()) return true;
            arrows.release_pending = false;
        }
        bool reset = false;
        if (!arrows.held_sample) {
            const esp_err_t err = solar_os_input_read_captured_pointer(arrows.source, &arrows,
                                                                       &arrows.sample, &reset);
            if (reset) {
                clear_motion();
                arrows.release_pending = true;
                arrows.resets++;
                arrows.skip_stroke = true;
                arrows.last_ms = err == ESP_OK ? arrows.sample.time_ms : event->data.tick_ms;
            }
            if (err == ESP_ERR_INVALID_STATE) {
                const uint32_t generation = arrows.generation;
                pointer_arrows_stop(ctx);
                (void)solar_os_jobs_mark_stopped("pointer-arrows", generation, err);
                return true;
            }
            if (err != ESP_OK) {
                drained = true;
                break;
            }
            arrows.held_sample = true;
            if (arrows.release_pending) {
                if (!release_key()) return true;
                arrows.release_pending = false;
            }
        }
        const solar_os_input_pointer_sample_t *sample = &arrows.sample;
        const solar_os_input_pointer_event_t *pointer = &sample->event;
        if (arrows.skip_stroke) {
            if (pointer->mode == SOLAR_OS_INPUT_POINTER_RELATIVE &&
                (int32_t)(sample->time_ms - arrows.last_ms) >= (int32_t)POINTER_ARROWS_QUIET_MS) {
                arrows.skip_stroke = false;
            } else {
                arrows.last_ms = sample->time_ms;
                if (pointer->action == SOLAR_OS_INPUT_POINTER_RELEASE)
                    arrows.skip_stroke = false;
                arrows.held_sample = false;
                continue;
            }
        }
        if (!advance_motion(sample->time_ms))
            return true;
        int32_t x = pointer->delta_x, y = pointer->delta_y;
        if (pointer->mode == SOLAR_OS_INPUT_POINTER_ABSOLUTE) {
            if (pointer->action == SOLAR_OS_INPUT_POINTER_PRESS) {
                if (!finish_motion())
                    return true;
                arrows.contact = true;
                arrows.pointer_id = pointer->pointer_id;
                arrows.absolute_x = pointer->x;
                arrows.absolute_y = pointer->y;
                x = y = 0;
            } else if (arrows.contact && pointer->pointer_id == arrows.pointer_id) {
                x = (int32_t)pointer->x - arrows.absolute_x;
                y = (int32_t)pointer->y - arrows.absolute_y;
                arrows.absolute_x = pointer->x;
                arrows.absolute_y = pointer->y;
            } else {
                x = y = 0;
            }
        }
        if (x || y) {
            if (!arrows.moving) arrows.window_ms = sample->time_ms;
            arrows.moving = true;
            arrows.x = add_delta(arrows.x, x);
            arrows.y = add_delta(arrows.y, y);
            arrows.horizontal = (x < 0 ? -x : x) >= (y < 0 ? -y : y);
            arrows.last_ms = sample->time_ms;
            arrows.modifiers = sample->modifiers_valid ? sample->modifiers
                                                       : solar_os_input_composition_modifiers();
            arrows.pending = true;
        }
        if (pointer->mode == SOLAR_OS_INPUT_POINTER_ABSOLUTE &&
            pointer->action == SOLAR_OS_INPUT_POINTER_RELEASE && arrows.contact &&
            pointer->pointer_id == arrows.pointer_id) {
            if (!finish_motion()) return true;
            arrows.contact = false;
        }
        arrows.held_sample = false;
    }
    if (drained) (void)advance_motion(event->data.tick_ms);
    return true;
}

static void pointer_arrows_detail(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io)
        solar_os_shell_io_table_printf(
            io, "  source=%s min-vector=%u capture=%s evaluations=%lu resets=%lu eval-ms=%u quiet-ms=%u\n", arrows.name,
            (unsigned)arrows.min_vector, arrows.running ? "exclusive" : "released", (unsigned long)arrows.emitted,
            (unsigned long)arrows.resets, POINTER_ARROWS_EVALUATE_MS, POINTER_ARROWS_QUIET_MS);
}

const solar_os_job_t solar_os_pointer_arrows_job = {
    .name = "pointer-arrows",
    .summary = "consume a pointer source and hold arrow keys while moving",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = pointer_arrows_start,
    .stop = pointer_arrows_stop,
    .event = pointer_arrows_event,
    .detail = pointer_arrows_detail,
    .tick_interval_ms = 10U,
};
