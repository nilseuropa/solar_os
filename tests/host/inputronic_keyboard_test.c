#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "solar_os_memory.h"
#include <string.h>

/* Exercise the actual worker, polling, attach and detach implementation. */
#include "../../src/services/solar_os_tca8418.c"
#include "../../src/services/solar_os_inputronic_keyboard.c"
#include "../../src/services/solar_os_inputronic_keyboard_driver.c"
#include "../../src/services/solar_os_lilygo_pager_keyboard.c"
#include "solar_os_keys.h"
#include "solar_os_inputronic_keyboard_profile.h"

static uint8_t registers[0x30], fifo[32];
static size_t fifo_count, event_count, released, opened, closed, ready_changes;
static solar_os_input_key_event_t events[64];
static esp_err_t bus_error, input_error;
static bool task_failure, wait_timeout, overflow_during_read, refill_fifo;
static unsigned worker_ticks, worker_limit;
static uint8_t composition_modifiers;
static unsigned matrix_configurations;
static uint8_t fail_write_reg;
static bool fail_source_open;
static unsigned pwm_started, pwm_stopped;
static unsigned task_notifications;
static bool sources_alive[256];
static char source_names[256][SOLAR_OS_INPUT_SOURCE_NAME_MAX];
void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t cls, const char *tag)
{
    (void)tag; assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED); return malloc(bytes);
}
static TickType_t test_clock;
TickType_t xTaskGetTickCount(void) { return test_clock; }
void vTaskDelay(TickType_t ticks) { test_clock += ticks; }
esp_err_t solar_os_storage_read_file(const char *path, void *data, size_t len, size_t *read_len)
{
    (void)path; (void)data; (void)len; (void)read_len; return ESP_ERR_NOT_SUPPORTED;
}
bool solar_os_input_source_get_info(solar_os_input_source_t source, solar_os_input_source_info_t *info)
{
    if (!sources_alive[source]) return false;
    memset(info, 0, sizeof(*info)); info->source = source;
    info->capabilities = SOLAR_OS_INPUT_CAP_KEY_EVENTS;
    strlcpy(info->name, source_names[source], sizeof(info->name));
    return true;
}
bool solar_os_input_source_find(const char *name, solar_os_input_source_info_t *info)
{
    for (unsigned i = 1; i < 256; i++)
        if (sources_alive[i] && strcmp(source_names[i], name) == 0)
            return solar_os_input_source_get_info((solar_os_input_source_t)i, info);
    return false;
}
static esp_err_t pwm_start_error, pwm_stop_error;

esp_err_t pwm_port_set(gpio_num_t pin, uint32_t frequency, uint8_t percent)
{
    assert(pin == 46 && frequency == 5000U && percent == 50U);
    pwm_started++;
    return pwm_start_error;
}

esp_err_t pwm_port_stop(gpio_num_t pin)
{
    assert(pin == 46);
    pwm_stopped++;
    return pwm_stop_error;
}
static bool check_reset_before_probe;
static esp_err_t gpio_error;
static unsigned reset_delays, reset_cleanup, irq_cleanup;
static uint32_t reset_level;

esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config->intr_type == GPIO_INTR_DISABLE);
    if (config->mode == GPIO_MODE_INPUT) {
        assert(config->pin_bit_mask == (1ULL << 40));
        assert(config->pull_up_en == GPIO_PULLUP_ENABLE);
    } else {
        assert(config->mode == GPIO_MODE_OUTPUT);
        assert(config->pin_bit_mask == (1ULL << 41) && reset_level == 0);
    }
    return gpio_error;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    assert(pin == 41);
    reset_level = level;
    return gpio_error;
}

esp_err_t gpio_reset_pin(gpio_num_t pin)
{
    assert(pin == 40 || pin == 41);
    if (pin == 41) reset_cleanup++;
    else irq_cleanup++;
    return ESP_OK;
}

void esp_rom_delay_us(uint32_t us)
{
    assert(us >= 120U);
    assert(reset_level == (reset_delays % 2U));
    reset_delays++;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t length = strlen(src);
    if (size != 0U) {
        size_t copied = length < size - 1U ? length : size - 1U;
        memcpy(dst, src, copied);
        dst[copied] = '\0';
    }
    return length;
}

bool solar_os_expansion_find_i2c_bus(const char *name,
    solar_os_expansion_i2c_bus_t *bus, size_t *index)
{
    (void)bus; (void)index;
    return strcmp(name, "i2c0") == 0 || strcmp(name, "i2c1") == 0;
}

esp_err_t solar_os_bus_i2c_probe(const char *name, uint8_t address)
{
    assert(name != NULL && address == 0x34U);
    if (check_reset_before_probe) assert(reset_level == 1U && reset_delays >= 2U);
    return bus_error;
}

esp_err_t solar_os_bus_i2c_read_reg(const char *name, uint8_t address,
    uint8_t reg, uint8_t *data, size_t length)
{
    assert(name != NULL && address == 0x34U && length == 1U);
    if (bus_error != ESP_OK) return bus_error;
    if (reg == REG_KEY_COUNT) *data = (uint8_t)fifo_count;
    else if (reg == REG_KEY_EVENT) {
        assert(fifo_count != 0U);
        *data = fifo[0];
        memmove(fifo, fifo + 1, --fifo_count);
        if (refill_fifo) fifo[fifo_count++] = 0x80 | 52;
        if (overflow_during_read) {
            registers[REG_INT_STAT] |= INT_OVERFLOW;
            overflow_during_read = false;
        }
    } else *data = registers[reg];
    return ESP_OK;
}

esp_err_t solar_os_bus_i2c_write_reg(const char *name, uint8_t address,
    uint8_t reg, const uint8_t *data, size_t length)
{
    assert(name != NULL && address == 0x34U && length == 1U);
    if (bus_error != ESP_OK) return bus_error;
    if (reg == fail_write_reg) return ESP_FAIL;
    if (reg == REG_INT_STAT) registers[reg] &= (uint8_t)~*data;
    else registers[reg] = *data;
    if (reg == REG_CFG && *data == 0x29U) matrix_configurations++;
    return ESP_OK;
}

esp_err_t solar_os_input_keyboard_source_open(const char *name, bool ready,
    solar_os_input_source_t *source)
{
    assert(name != NULL && ready);
    if (fail_source_open) return ESP_ERR_NO_MEM;
    *source = (solar_os_input_source_t)++opened;
    sources_alive[*source] = true;
    strlcpy(source_names[*source], name, sizeof(source_names[*source]));
    return ESP_OK;
}

void solar_os_input_source_close(solar_os_input_source_t source)
{
    assert(source != 0U);
    sources_alive[source] = false;
    closed++;
}

void solar_os_input_source_release_all(solar_os_input_source_t source)
{
    assert(source != 0U);
    released++;
}

esp_err_t solar_os_input_keyboard_source_set_ready(solar_os_input_source_t source, bool ready)
{
    assert(source != 0U);
    (void)ready;
    ready_changes++;
    return ESP_OK;
}

void solar_os_input_composition_set_modifiers(solar_os_input_source_t source, uint8_t modifiers)
{
    assert(source != 0U);
    composition_modifiers = modifiers;
}

uint8_t solar_os_input_composition_apply(uint8_t key) { return key; }

esp_err_t solar_os_input_write_char(solar_os_input_source_t source, char key)
{
    assert(source != 0 && key != 0);
    return input_error;
}

/* The common translator itself is covered by input_test. Verify what this
 * driver passes to it; exercise a letter and modifier navigation here. */
uint8_t solar_os_input_translate_hid_usage(uint16_t usage, uint8_t modifiers, bool caps)
{
    if (usage >= 0x04U && usage <= 0x1dU) {
        uint8_t key = (uint8_t)('a' + usage - 0x04U);
        if ((modifiers & SOLAR_OS_INPUT_MOD_CTRL) != 0U) return (uint8_t)(key - 'a' + 1U);
        if (((modifiers & SOLAR_OS_INPUT_MOD_SHIFT) != 0U) != caps) key -= 'a' - 'A';
        return key;
    }
    if (usage == 0x52U) return (modifiers & SOLAR_OS_INPUT_MOD_SHIFT) != 0U ?
        SOLAR_OS_KEY_SHIFT_UP : SOLAR_OS_KEY_UP;
    return 0U;
}

esp_err_t solar_os_input_write_key(solar_os_input_source_t source, uint16_t physical,
    uint16_t usage, uint8_t key, uint8_t modifiers, solar_os_input_key_action_t action)
{
    assert(source != 0U);
    if (input_error != ESP_OK) return input_error;
    assert(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = (solar_os_input_key_event_t) {
        .source = source, .physical_key = physical, .usage = usage, .key = key,
        .modifiers = modifiers, .action = action,
    };
    return ESP_OK;
}

BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t function, const char *name,
    uint32_t stack, void *arg, UBaseType_t priority, TaskHandle_t *task,
    BaseType_t core, solar_os_task_role_t role)
{
    assert(function == worker && name != NULL && stack == TCA8418_TASK_STACK);
    (void)priority; (void)core; (void)role;
    *task = arg;
    return task_failure ? pdFALSE : pdPASS;
}

void solar_os_task_delete_internal(TaskHandle_t task) { assert(task == NULL); }
BaseType_t xTaskNotifyGive(TaskHandle_t task) { assert(task != NULL); task_notifications++; return pdPASS; }

bool solar_os_task_wait_done(TaskHandle_t task, volatile bool *done, uint32_t timeout)
{
    assert(timeout == SOLAR_OS_TASK_STOP_WAIT_MS);
    if (wait_timeout) return false;
    worker(task);
    return *done;
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    assert(clear == pdTRUE);
    assert(ticks == (worker_ticks == 0U ? TCA8418_RETRY_MS : TCA8418_POLL_MS));
    worker_ticks++;
    /* A failed first poll recovers on the next iteration, then accepts a
     * fresh key. No held Shift survives the connection interruption. */
    if (worker_ticks == 1U) bus_error = ESP_OK;
    if (worker_ticks == 2U) { fifo[0] = 0x80U | 52U; fifo_count = 1U; }
    if (worker_ticks >= worker_limit) devices[0].stop_requested = true;
    return 0U;
}

static void queue(uint8_t raw)
{
    assert(fifo_count < sizeof(fifo));
    fifo[fifo_count++] = raw;
    registers[REG_INT_STAT] |= INT_KEY;
}

static void test_codec(void)
{
    solar_os_matrix_keyboard_map_t map;
    solar_os_inputronic_keyboard_map(&map);
    solar_os_matrix_keyboard_state_t state = {0};
    solar_os_matrix_key_transition_t transition;
    assert(!solar_os_matrix_keyboard_decode(NULL, &map, 0x80, &transition));
    assert(!solar_os_matrix_keyboard_decode(&state, &map, 0, &transition));
    assert(!solar_os_matrix_keyboard_decode(&state, &map, 0xd1, &transition));
    assert(!solar_os_matrix_keyboard_decode(&state, &map, 0x81, &transition));
    assert(!solar_os_matrix_keyboard_decode(&state, &map, 52, &transition));
    assert(solar_os_matrix_keyboard_decode(&state, &map, 0x80 | 74, &transition));
    assert(state.caps_lock && transition.usage == 0x39);
    assert(!solar_os_matrix_keyboard_decode(&state, &map, 0x80 | 74, &transition));
    assert(state.caps_lock);
    assert(solar_os_matrix_keyboard_decode(&state, &map, 74, &transition));
    assert(solar_os_matrix_keyboard_decode(&state, &map, 0x80 | 75, &transition));
    assert(transition.modifiers == SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    for (uint8_t id = 31; id <= 40; id++) {
        static const char expected[] = "=!\"#$%&/()";
        assert(solar_os_matrix_keyboard_decode(&state, &map, 0x80 | id, &transition));
        assert(transition.key == expected[id - 31]);
    }
    assert(solar_os_matrix_keyboard_decode(&state, &map, 0x80 | 51, &transition));
    assert(transition.key == ':');
    assert(solar_os_matrix_keyboard_decode(&state, &map, 0x80 | 69, &transition));
    assert(transition.key == ';');
    assert(solar_os_matrix_keyboard_decode(&state, &map, 0x80 | 70, &transition));
    assert(transition.key == ':');
    assert(solar_os_matrix_keyboard_decode(&state, &map, 0x80 | 76, &transition));
    assert(transition.modifiers == (SOLAR_OS_INPUT_MOD_LEFT_SHIFT | SOLAR_OS_INPUT_MOD_LEFT_CTRL));
    assert(solar_os_matrix_keyboard_decode(&state, &map, 0x80 | 77, &transition));
    assert(transition.modifiers == 7U);
    solar_os_matrix_keyboard_release(&state);
    assert(state.caps_lock && state.modifiers == 0U && !state.held[75]);
    for (uint8_t id = 17; id <= 20; id++) {
        assert(solar_os_matrix_keyboard_decode(&state, &map, 0x80 | id, &transition));
        assert(transition.physical_key == id && transition.key == 0 && transition.usage == 0);
    }
    assert(solar_os_matrix_keyboard_decode(&state, &map, 0x80 | 78, &transition));
    assert(solar_os_matrix_keyboard_decode(&state, &map, 0x80 | 79, &transition));
    for (uint8_t id = 21; id <= 30; id++) {
        assert(solar_os_matrix_keyboard_decode(&state, &map, 0x80 | id, &transition));
        assert(transition.usage == 0x3a + id - 21);
    }
    /* Every printable letter's physical ID is independent of its HID usage. */
    static const uint8_t ids[] = {52,66,64,54,44,55,56,57,49,58,59,60,68,67,50,41,42,45,53,46,48,65,43,63,47,62};
    for (size_t i = 0; i < sizeof(ids); i++) {
        assert(solar_os_matrix_keyboard_decode(&state, &map, 0x80 | ids[i], &transition));
        assert(transition.usage == 4U + i);
    }
}

int main(void)
{
    test_codec();
    solar_os_expansion_binding_t bindings[] = {
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target = "i2c0"},
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value = 0x34},
    };
    assert(solar_os_inputronic_keyboard_attach(NULL, bindings, 2) == ESP_ERR_INVALID_ARG);
    assert(solar_os_inputronic_keyboard_attach("keyboard0", bindings, 1) == ESP_ERR_INVALID_ARG);
    bindings[1].value = 0x35;
    assert(solar_os_inputronic_keyboard_attach("keyboard0", bindings, 2) == ESP_ERR_INVALID_ARG);
    bindings[1].value = 0x34;
    bus_error = ESP_FAIL;
    assert(solar_os_inputronic_keyboard_attach("keyboard0", bindings, 2) == ESP_FAIL);
    assert(opened == 0U);
    bus_error = ESP_OK;
    fail_write_reg = 0x1f;
    assert(solar_os_inputronic_keyboard_attach("keyboard0", bindings, 2) == ESP_FAIL);
    assert(opened == 0U && !devices[0].active);
    fail_write_reg = 0U;
    fail_source_open = true;
    assert(solar_os_inputronic_keyboard_attach("keyboard0", bindings, 2) == ESP_ERR_NO_MEM);
    assert(opened == 0U && !devices[0].active);
    fail_source_open = false;
    queue(0x80 | 52); /* Attach discards stale events. */
    task_failure = true;
    assert(solar_os_inputronic_keyboard_attach("keyboard0", bindings, 2) == ESP_ERR_NO_MEM);
    assert(opened == closed && !devices[0].active);
    task_failure = false;
    assert(solar_os_inputronic_keyboard_expansion_driver.attach("keyboard0", bindings, 2) == ESP_OK);
    assert(registers[0x1d] == 0xff && registers[0x1e] == 0xff && registers[0x1f] == 3);
    assert(registers[REG_CFG] == 0x29 && registers[0x29] == 0 && fifo_count == 0);
    assert(solar_os_inputronic_keyboard_attach("keyboard0", bindings, 2) == ESP_ERR_INVALID_STATE);
    tca8418_device_t *device = &devices[0];
    queue(0x80 | 75); queue(0x80 | 52); queue(52); queue(75);
    assert(poll_once(device) == ESP_OK);
    assert(event_count == 4 && events[1].key == 'A' && events[1].usage == 4);
    assert(events[2].action == SOLAR_OS_INPUT_KEY_RELEASE && composition_modifiers == 0);
    queue(0x80 | 76); queue(0x80 | 52); queue(52); queue(76);
    assert(poll_once(device) == ESP_OK && events[5].key == 1);
    queue(0x80 | 74); queue(74); queue(0x80 | 52); queue(52);
    assert(poll_once(device) == ESP_OK && events[10].key == 'A');
    queue(0x80 | 75); queue(0x80 | 7); queue(7); queue(75);
    assert(poll_once(device) == ESP_OK && events[13].key == SOLAR_OS_KEY_SHIFT_UP);
    queue(0x80 | 75); queue(0x80 | 52); queue(52); queue(75);
    assert(poll_once(device) == ESP_OK && events[17].key == 'a'); /* Caps XOR Shift. */
    queue(0x80 | 75);
    assert(poll_once(device) == ESP_OK && composition_modifiers != 0);
    size_t prior_events = event_count;
    queue(75); registers[REG_INT_STAT] |= INT_OVERFLOW;
    assert(poll_once(device) == ESP_OK && event_count == prior_events);
    assert(device->overflows == 1 && released == 1 && composition_modifiers == 0);
    assert(fifo_count == 0);
    queue(0x80 | 52); overflow_during_read = true;
    assert(poll_once(device) == ESP_OK && event_count == prior_events);
    assert(device->overflows == 2 && released == 2);
    queue(0x80 | 52); refill_fifo = true;
    assert(discard_fifo(device) == ESP_ERR_TIMEOUT);
    refill_fifo = false;
    assert(discard_fifo(device) == ESP_OK && fifo_count == 0);
    registers[REG_INT_STAT] = 0;
    /* Count is polled even when K_INT was acknowledged concurrently. */
    queue(0x80 | 52); registers[REG_INT_STAT] = 0;
    assert(poll_once(device) == ESP_OK && events[event_count - 1].key == 'A');
    queue(52);
    assert(poll_once(device) == ESP_OK);
    input_error = ESP_ERR_NO_MEM; queue(0x80 | 75);
    assert(poll_once(device) == ESP_ERR_NO_MEM);
    input_error = ESP_OK;
    unsigned prior_configurations = matrix_configurations;
    bus_error = ESP_FAIL; worker_limit = 3;
    worker(device);
    assert(ready_changes == 2 && matrix_configurations == prior_configurations + 1);
    assert(composition_modifiers == 0);
    assert(events[event_count - 1].key == 'A');
    /* Multiple independent instances on different named buses. */
    strlcpy(bindings[0].target, "i2c1", sizeof(bindings[0].target));
    assert(solar_os_inputronic_keyboard_attach("keyboard1", bindings, 2) == ESP_OK);
    assert(devices[1].source != device->source);
    wait_timeout = true;
    assert(solar_os_inputronic_keyboard_detach("keyboard1") == ESP_ERR_TIMEOUT);
    assert(devices[1].active); /* Registry must retain the bus/address lease. */
    wait_timeout = false;
    assert(solar_os_inputronic_keyboard_detach("keyboard1") == ESP_OK);
    assert(solar_os_inputronic_keyboard_detach("keyboard0") == ESP_OK);
    assert(opened == closed);
    assert(solar_os_inputronic_keyboard_detach("missing") == ESP_ERR_NOT_FOUND);

    /* A keyboard held in reset must be released BEFORE its first I2C probe.
     * Failed initialization and detach must relinquish both GPIOs. */
    solar_os_expansion_binding_t wired[] = {
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target = "i2c0"},
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value = 0x34},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset", .value = 41},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "irq", .value = 40},
    };
    check_reset_before_probe = true;
    bus_error = ESP_ERR_NOT_FOUND;
    assert(solar_os_inputronic_keyboard_attach("wired", wired, 4) == ESP_ERR_NOT_FOUND);
    assert(reset_delays == 2U && reset_cleanup == 1U && irq_cleanup == 1U);
    gpio_error = ESP_FAIL;
    assert(solar_os_inputronic_keyboard_attach("wired", wired, 4) == ESP_FAIL);
    assert(reset_delays == 2U && reset_cleanup == 2U && irq_cleanup == 2U);
    gpio_error = ESP_OK;
    bus_error = ESP_OK;
    assert(solar_os_inputronic_keyboard_attach("wired", wired, 4) == ESP_OK);
    assert(reset_delays == 4U && reset_level == 1U);
    assert(solar_os_inputronic_keyboard_detach("wired") == ESP_OK);
    assert(reset_cleanup == 3U && irq_cleanup == 3U && opened == closed);
    wired[2].value = -1;
    assert(solar_os_inputronic_keyboard_attach("wired", wired, 4) == ESP_ERR_INVALID_ARG);
    wired[2].value = 40;
    assert(solar_os_inputronic_keyboard_attach("wired", wired, 4) == ESP_ERR_INVALID_ARG);

    /* Generic geometry and live remapping use the same controller worker. */
    check_reset_before_probe = false;
    solar_os_expansion_binding_t generic_bindings[] = {
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target="i2c0"},
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value=0x34},
        {.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role="rows", .value=3},
        {.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role="cols", .value=3},
    };
    assert(solar_os_tca8418_attach("generic", generic_bindings, 4) == ESP_OK);
    assert(registers[0x1d] == 7 && registers[0x1e] == 7 && registers[0x1f] == 0);
    tca8418_device_t *generic_device = &devices[0];
    prior_events = event_count;
    queue(0x80 | 11); queue(11); queue(0x80 | 4); /* Narrow matrices still have stride 10. */
    assert(poll_once(generic_device) == ESP_OK && event_count == prior_events + 2);
    assert(events[prior_events].key == 'a');
    solar_os_matrix_keyboard_map_t custom, before;
    assert(solar_os_tca8418_get_keymap("generic", true, &custom) == ESP_OK);
    before = custom;
    custom.keys[0][1].usage = 4; /* Q position becomes A. */
    queue(0x80 | 21); /* Hold Shift while changing the map. */
    assert(poll_once(generic_device) == ESP_OK);
    assert(composition_modifiers != 0);
    queue(21); /* Old queued release is discarded at the map boundary. */
    size_t prior_released = released;
    assert(solar_os_tca8418_set_keymap("generic", &custom) == ESP_OK);
    assert(released == prior_released + 1 && composition_modifiers == 0 && fifo_count == 0);
    queue(0x80 | 1); queue(1);
    assert(poll_once(generic_device) == ESP_OK && events[event_count - 2].key == 'a');
    custom.keys[0][4].usage = 4; /* Outside configured columns. */
    prior_released = released;
    assert(solar_os_tca8418_set_keymap("generic", &custom) == ESP_ERR_INVALID_ARG);
    assert(released == prior_released);
    assert(solar_os_tca8418_reset_keymap("generic") == ESP_OK);
    assert(solar_os_tca8418_get_keymap("generic", false, &custom) == ESP_OK);
    assert(memcmp(&before, &custom, sizeof(custom)) == 0);
    bus_error = ESP_FAIL;
    assert(solar_os_tca8418_set_keymap("generic", &custom) == ESP_FAIL);
    assert(generic_device->recovering && composition_modifiers == 0);
    bus_error = ESP_OK;
    generic_device->stop_requested = true;
    worker(generic_device);
    unsigned prior_notifications = task_notifications;
    assert(solar_os_tca8418_detach("generic") == ESP_OK);
    assert(task_notifications == prior_notifications); /* A finished task must not be notified again. */
    assert(solar_os_tca8418_get_keymap("generic", false, &custom) == ESP_ERR_NOT_FOUND);

    /* Pager owns PWM independently. A failed worker stop retains PWM;
     * a failed PWM stop can be retried after the controller has stopped. */
    solar_os_expansion_binding_t pager_bindings[] = {
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target="i2c0"},
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value=0x34},
        {.kind=SOLAR_OS_EXPANSION_BINDING_PWM, .role="backlight", .value=46},
    };
    pwm_start_error = ESP_FAIL;
    assert(solar_os_lilygo_pager_keyboard_attach("pager", pager_bindings, 3) == ESP_FAIL);
    assert(!pager_devices[0].active && pwm_stopped == pwm_started);
    pwm_start_error = ESP_OK;
    bus_error = ESP_ERR_NOT_FOUND;
    assert(solar_os_lilygo_pager_keyboard_attach("pager", pager_bindings, 3) == ESP_ERR_NOT_FOUND);
    assert(!pager_devices[0].active && pwm_stopped == pwm_started);
    bus_error = ESP_OK;
    assert(solar_os_lilygo_pager_keyboard_attach("pager", pager_bindings, 3) == ESP_OK);
    assert(registers[0x1d] == 15 && registers[0x1e] == 255 && registers[0x1f] == 3);
    unsigned prior_stopped = pwm_stopped;
    wait_timeout = true;
    assert(solar_os_lilygo_pager_keyboard_detach("pager") == ESP_ERR_TIMEOUT);
    assert(pwm_stopped == prior_stopped && pager_devices[0].controller_attached);
    wait_timeout = false;
    pwm_stop_error = ESP_FAIL;
    assert(solar_os_lilygo_pager_keyboard_detach("pager") == ESP_FAIL);
    assert(pager_devices[0].active && !pager_devices[0].controller_attached);
    size_t prior_closed = closed;
    pwm_stop_error = ESP_OK;
    assert(solar_os_lilygo_pager_keyboard_detach("pager") == ESP_OK);
    assert(closed == prior_closed && !pager_devices[0].active && opened == closed);

    puts("TCA8418 keyboard lifecycle tests: ok");
    return 0;
}
