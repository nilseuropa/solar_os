#include "solar_os_buttons.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "solar_os_board_caps.h"
#include "solar_os_input.h"
#include "solar_os_input_composition.h"
#include "solar_os_log.h"

#if SOLAR_OS_BOARD_HAS_BUTTONS
#include "solar_os_board.h"

#ifndef SOLAR_OS_BOARD_BUTTONS
#error "Board enables BUTTONS but does not define SOLAR_OS_BOARD_BUTTONS."
#endif

#define BUTTON_DEBOUNCE_MS 25U
#define BUTTON_EDGE_QUEUE_SIZE 64U
#define BUTTON_MOTION_QUIET_MS 80U

typedef struct {
    uint32_t time_ms;
    uint8_t index;
    uint8_t modifiers;
} button_edge_t;

typedef struct {
    bool initialized;
    bool last_raw_pressed;
    bool stable_pressed;
    bool edge_isr_installed;
    uint32_t raw_changed_ms;
    uint32_t raw_transitions;
    uint32_t stable_transitions;
    uint32_t dropped_edges;
} button_state_t;

static const char *TAG = "solar_os_buttons";
static const solar_os_button_def_t button_defs[] = SOLAR_OS_BOARD_BUTTONS;
static button_state_t button_states[sizeof(button_defs) / sizeof(button_defs[0])];
static bool buttons_initialized;
static solar_os_input_source_t buttons_input_source;
static portMUX_TYPE buttons_edge_lock = portMUX_INITIALIZER_UNLOCKED;
static button_edge_t buttons_edges[BUTTON_EDGE_QUEUE_SIZE];
static size_t buttons_edge_head, buttons_edge_count;
static uint32_t buttons_last_edge_ms;
static int32_t buttons_motion_x, buttons_motion_y;
static uint32_t buttons_motion_last_ms;
static uint8_t buttons_motion_modifiers;
static bool buttons_motion_pending, buttons_motion_horizontal;
_Static_assert(sizeof(button_defs) / sizeof(button_defs[0]) <= UINT8_MAX,
               "Button edge indices must fit in uint8_t");

static uint32_t buttons_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static gpio_pullup_t button_pull_up(solar_os_button_pull_t pull)
{
    return pull == SOLAR_OS_BUTTON_PULL_UP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
}

static gpio_pulldown_t button_pull_down(solar_os_button_pull_t pull)
{
    return pull == SOLAR_OS_BUTTON_PULL_DOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
}

static bool button_raw_pressed(const solar_os_button_def_t *def)
{
    const int level = gpio_get_level(def->pin);
    return def->active_low ? level == 0 : level != 0;
}

static void button_edge_isr(void *arg)
{
    const solar_os_button_def_t *def = arg;
    const size_t index = (size_t)(def - button_defs);
    const bool pressed = button_raw_pressed(def);
    const uint32_t time_ms = buttons_millis();
    /* Composition exposes a bounded, lock-free read of volatile modifier
     * bytes. Snapshot here so a keyboard release before polling cannot change
     * the meaning of an already-started gesture. Dispatch stays in task context. */
    const uint8_t modifiers = solar_os_input_composition_modifiers();
    portENTER_CRITICAL_ISR(&buttons_edge_lock);
    buttons_last_edge_ms = time_ms;
    button_state_t *state = &button_states[index];
    state->last_raw_pressed = pressed;
    state->raw_transitions++;
    if (buttons_edge_count < BUTTON_EDGE_QUEUE_SIZE) {
        buttons_edges[(buttons_edge_head + buttons_edge_count) % BUTTON_EDGE_QUEUE_SIZE] =
            (button_edge_t){.index = (uint8_t)index, .time_ms = time_ms,
                            .modifiers = modifiers};
        buttons_edge_count++;
    } else {
        state->dropped_edges++;
    }
    portEXIT_CRITICAL_ISR(&buttons_edge_lock);
}

static esp_err_t button_write_tap(const solar_os_button_def_t *def, uint8_t modifiers)
{
    const uint8_t key = solar_os_input_composition_apply_with_modifiers(def->key, modifiers);
    /* Preserve keyboard modifiers for Alt+Left/Right. Translated board chords
     * such as Page Up already encode their modifiers in the replacement key. */
    if (key != def->key) {
        modifiers = 0U;
    }
    return solar_os_input_write_key_tap(buttons_input_source,
        SOLAR_OS_INPUT_PHYSICAL_NONE, SOLAR_OS_INPUT_USAGE_NONE, key, modifiers);
}

static bool button_finish_motion(void)
{
    const int32_t x = buttons_motion_x < 0 ? -buttons_motion_x : buttons_motion_x;
    const int32_t y = buttons_motion_y < 0 ? -buttons_motion_y : buttons_motion_y;
    if (x != 0 || y != 0) {
        /* Equal diagonals use the most recently moved axis. Opposite movement
         * cancels on each axis, just as it would for a relative pointer. */
        const bool horizontal = x > y || (x == y && buttons_motion_horizontal);
        const uint8_t key = horizontal ?
            (buttons_motion_x < 0 ? SOLAR_OS_KEY_LEFT : SOLAR_OS_KEY_RIGHT) :
            (buttons_motion_y < 0 ? SOLAR_OS_KEY_UP : SOLAR_OS_KEY_DOWN);
        for (size_t i = 0; i < sizeof(button_defs) / sizeof(button_defs[0]); i++) {
            if (button_defs[i].track_motion && button_defs[i].key == key) {
                if (button_write_tap(&button_defs[i], buttons_motion_modifiers) != ESP_OK) {
                    return false;
                }
                break;
            }
        }
    }
    buttons_motion_x = buttons_motion_y = 0;
    buttons_motion_pending = false;
    return true;
}

static void button_accumulate_motion(uint8_t key)
{
    /* Saturate pathological, never-ending strokes without signed overflow. */
    switch (key) {
    case SOLAR_OS_KEY_LEFT:
        if (buttons_motion_x > -INT32_MAX) buttons_motion_x--;
        buttons_motion_horizontal = true;
        break;
    case SOLAR_OS_KEY_RIGHT:
        if (buttons_motion_x < INT32_MAX) buttons_motion_x++;
        buttons_motion_horizontal = true;
        break;
    case SOLAR_OS_KEY_UP:
        if (buttons_motion_y > -INT32_MAX) buttons_motion_y--;
        buttons_motion_horizontal = false;
        break;
    case SOLAR_OS_KEY_DOWN:
        if (buttons_motion_y < INT32_MAX) buttons_motion_y++;
        buttons_motion_horizontal = false;
        break;
    }
}

static void button_stop_edges(void)
{
    for (size_t i = 0; i < sizeof(button_defs) / sizeof(button_defs[0]); i++) {
        if (button_states[i].edge_isr_installed) {
            (void)gpio_set_intr_type(button_defs[i].pin, GPIO_INTR_DISABLE);
            (void)gpio_isr_handler_remove(button_defs[i].pin);
            button_states[i].edge_isr_installed = false;
        }
    }
    portENTER_CRITICAL(&buttons_edge_lock);
    buttons_edge_head = buttons_edge_count = 0;
    portEXIT_CRITICAL(&buttons_edge_lock);
}
#endif

esp_err_t solar_os_buttons_init(void)
{
#if !SOLAR_OS_BOARD_HAS_BUTTONS
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (buttons_initialized) {
        return ESP_OK;
    }

    for (size_t i = 0; i < sizeof(button_defs) / sizeof(button_defs[0]); i++) {
        const solar_os_button_def_t *def = &button_defs[i];
        if (!GPIO_IS_VALID_GPIO(def->pin) ||
            (def->track_motion && (def->emit_on_release ||
                (def->key != SOLAR_OS_KEY_LEFT && def->key != SOLAR_OS_KEY_RIGHT &&
                 def->key != SOLAR_OS_KEY_UP && def->key != SOLAR_OS_KEY_DOWN)))) {
            return ESP_ERR_INVALID_ARG;
        }

        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << (uint32_t)def->pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = button_pull_up(def->pull),
            .pull_down_en = button_pull_down(def->pull),
            .intr_type = GPIO_INTR_DISABLE,
        };
        const esp_err_t ret = gpio_config(&config);
        if (ret != ESP_OK) {
            return ret;
        }

        const bool pressed = button_raw_pressed(def);
        button_states[i] = (button_state_t) {
            .initialized = true,
            .last_raw_pressed = pressed,
            .stable_pressed = pressed,
            .raw_changed_ms = buttons_millis(),
        };
    }

    const esp_err_t input_err =
        solar_os_input_key_source_open("buttons",
                                       SOLAR_OS_INPUT_SOURCE_BUTTONS,
                                       &buttons_input_source);
    if (input_err != ESP_OK) {
        return input_err;
    }

    esp_err_t edge_err = ESP_OK;
    for (size_t i = 0; i < sizeof(button_defs) / sizeof(button_defs[0]); i++) {
        if (!button_defs[i].track_motion) {
            continue;
        }
        edge_err = gpio_install_isr_service(0);
        if (edge_err != ESP_OK && edge_err != ESP_ERR_INVALID_STATE) {
            break;
        }
        edge_err = gpio_isr_handler_add(button_defs[i].pin, button_edge_isr,
                                       (void *)&button_defs[i]);
        if (edge_err != ESP_OK) {
            break;
        }
        button_states[i].edge_isr_installed = true;
        edge_err = gpio_set_intr_type(button_defs[i].pin, GPIO_INTR_ANYEDGE);
        if (edge_err != ESP_OK) {
            break;
        }
    }
    if (edge_err != ESP_OK) {
        button_stop_edges();
        solar_os_input_source_close(buttons_input_source);
        return edge_err;
    }
    buttons_initialized = true;
    SOLAR_OS_LOGI(TAG, "%u board buttons ready", (unsigned)(sizeof(button_defs) / sizeof(button_defs[0])));
    return ESP_OK;
#endif
}

void solar_os_buttons_poll(void)
{
#if !SOLAR_OS_BOARD_HAS_BUTTONS
    return;
#else
    if (!buttons_initialized) {
        return;
    }

    const uint32_t now_ms = buttons_millis();

    /* Timestamped edges preserve separate strokes even if rendering delayed
     * polling across a pause. Leave an edge queued if input is backpressured. */
    for (size_t n = 0; n < BUTTON_EDGE_QUEUE_SIZE; n++) {
        portENTER_CRITICAL(&buttons_edge_lock);
        const bool empty = buttons_edge_count == 0;
        const uint32_t last_edge_ms = buttons_last_edge_ms;
        const button_edge_t edge = empty ? (button_edge_t){0} : buttons_edges[buttons_edge_head];
        portEXIT_CRITICAL(&buttons_edge_lock);
        if (empty) {
            if (buttons_motion_pending &&
                (uint32_t)(buttons_millis() - last_edge_ms) >= BUTTON_MOTION_QUIET_MS) {
                (void)button_finish_motion();
            }
            break;
        }
        if (buttons_motion_pending &&
            (uint32_t)(edge.time_ms - buttons_motion_last_ms) >= BUTTON_MOTION_QUIET_MS &&
            !button_finish_motion()) {
            break;
        }
        if (!buttons_motion_pending) {
            buttons_motion_pending = true;
        }
        /* The keyboard scan may publish a held modifier after the first GPIO
         * pulse. Use the latest movement sample, then preserve it while quiet. */
        buttons_motion_modifiers = edge.modifiers;
        button_accumulate_motion(button_defs[edge.index].key);
        buttons_motion_last_ms = edge.time_ms;
        portENTER_CRITICAL(&buttons_edge_lock);
        buttons_edge_head = (buttons_edge_head + 1U) % BUTTON_EDGE_QUEUE_SIZE;
        buttons_edge_count--;
        button_states[edge.index].stable_transitions++;
        button_states[edge.index].stable_pressed = button_states[edge.index].last_raw_pressed;
        portEXIT_CRITICAL(&buttons_edge_lock);
    }

    for (size_t i = 0; i < sizeof(button_defs) / sizeof(button_defs[0]); i++) {
        const solar_os_button_def_t *def = &button_defs[i];
        button_state_t *state = &button_states[i];
        if (def->track_motion) {
            continue;
        }
        const bool pressed = button_raw_pressed(def);

        if (!state->initialized) {
            *state = (button_state_t) {
                .initialized = true,
                .last_raw_pressed = pressed,
                .stable_pressed = pressed,
                .raw_changed_ms = now_ms,
            };
            continue;
        }

        if (pressed != state->last_raw_pressed) {
            state->last_raw_pressed = pressed;
            state->raw_changed_ms = now_ms;
            state->raw_transitions++;
            continue;
        }

        if (pressed == state->stable_pressed ||
            (uint32_t)(now_ms - state->raw_changed_ms) < BUTTON_DEBOUNCE_MS) {
            continue;
        }

        state->stable_pressed = pressed;
        state->stable_transitions++;
        if (def->key == 0) {
            continue;
        }
        if (def->emit_on_release) {
            if (!pressed) {
                (void)button_write_tap(def, solar_os_input_composition_modifiers());
            }
            continue;
        }
        (void)solar_os_input_write_key(buttons_input_source,
                                       (uint16_t)def->pin + 1U,
                                       SOLAR_OS_INPUT_USAGE_NONE,
                                       def->key,
                                       0,
                                       pressed ? SOLAR_OS_INPUT_KEY_PRESS :
                                           SOLAR_OS_INPUT_KEY_RELEASE);
    }
#endif
}

size_t solar_os_buttons_count(void)
{
#if !SOLAR_OS_BOARD_HAS_BUTTONS
    return 0U;
#else
    return sizeof(button_defs) / sizeof(button_defs[0]);
#endif
}

bool solar_os_buttons_debug_get(size_t index, solar_os_button_debug_info_t *info)
{
#if !SOLAR_OS_BOARD_HAS_BUTTONS
    (void)index;
    (void)info;
    return false;
#else
    if (info == NULL || index >= solar_os_buttons_count()) {
        return false;
    }
    const solar_os_button_def_t *def = &button_defs[index];
    const button_state_t *state = &button_states[index];
    portENTER_CRITICAL(&buttons_edge_lock);
    *info = (solar_os_button_debug_info_t) {
        .pin = def->pin,
        .name = def->name,
        .key = def->key,
        .raw_pressed = state->last_raw_pressed,
        .stable_pressed = state->stable_pressed,
        .raw_transitions = state->raw_transitions,
        .stable_transitions = state->stable_transitions,
        .dropped_edges = state->dropped_edges,
    };
    portEXIT_CRITICAL(&buttons_edge_lock);
    return true;
#endif
}

size_t solar_os_buttons_read_chars(char *buffer, size_t buffer_len)
{
    solar_os_buttons_poll();
#if !SOLAR_OS_BOARD_HAS_BUTTONS
    (void)buffer;
    (void)buffer_len;
    return 0;
#else
    return solar_os_input_read_source_chars(buttons_input_source, buffer, buffer_len);
#endif
}
