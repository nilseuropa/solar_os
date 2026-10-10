#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exercise the actual producer and lifecycle with the common keymap service. */
#define portENTER_CRITICAL_ISR(lock) ((void)(lock))
#define portEXIT_CRITICAL_ISR(lock) ((void)(lock))
#include "../../src/services/solar_os_tab5_keyboard.c"
#include "../../src/services/solar_os_tab5_keyboard_driver.c"
#include "solar_os_keys.h"

static uint8_t regs[256], fifo[64];
static unsigned fifo_count, read_events, event_count, opened, closed, released;
static unsigned notifications, irq_notifications, pin_resets, worker_ticks;
static unsigned allocations, fail_allocation;
static unsigned ready_ups;
static int fail_write = -1, fail_read = -1, fail_gpio_step, gpio_step;
static esp_err_t bus_error, input_error;
static bool task_failure, wait_timeout, source_failure, ready, empty_race, refill;
static unsigned worker_limit, last_wait;
static solar_os_input_key_event_t events[256];
static bool alive[256];
static char source_names[256][SOLAR_OS_INPUT_SOURCE_NAME_MAX];
static void (*gpio_handler)(void *);
static void *gpio_context;
static bool irq_enabled;
static TickType_t ticks;

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size != 0U) { size_t n = len < size - 1U ? len : size - 1U; memcpy(dst, src, n); dst[n] = 0; }
    return len;
}
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag != NULL);
    return ++allocations == fail_allocation ? NULL : malloc(size);
}
TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(TickType_t delay) { ticks += delay; }
esp_err_t solar_os_storage_read_file(const char *path, void *data, size_t len, size_t *read_len)
{ (void)path; (void)data; (void)len; (void)read_len; return ESP_ERR_NOT_SUPPORTED; }
bool solar_os_input_source_get_info(solar_os_input_source_t source, solar_os_input_source_info_t *info)
{
    if (!alive[source]) return false;
    memset(info, 0, sizeof(*info)); info->source = source;
    info->capabilities = SOLAR_OS_INPUT_CAP_KEY_EVENTS;
    strlcpy(info->name, source_names[source], sizeof(info->name));
    return true;
}
bool solar_os_input_source_find(const char *name, solar_os_input_source_info_t *info)
{
    for (unsigned i = 1; i < 256U; i++)
        if (alive[i] && strcmp(name, source_names[i]) == 0)
            return solar_os_input_source_get_info((solar_os_input_source_t)i, info);
    return false;
}
esp_err_t solar_os_input_keyboard_source_open(const char *name, bool is_ready,
    solar_os_input_source_t *source)
{
    assert(is_ready);
    if (source_failure) return ESP_ERR_NO_MEM;
    *source = (solar_os_input_source_t)++opened; alive[*source] = true;
    strlcpy(source_names[*source], name, sizeof(source_names[*source]));
    return ESP_OK;
}
void solar_os_input_source_close(solar_os_input_source_t source)
{ assert(alive[source]); alive[source] = false; closed++; }
void solar_os_input_source_release_all(solar_os_input_source_t source)
{ assert(alive[source]); released++; }
esp_err_t solar_os_input_keyboard_source_set_ready(solar_os_input_source_t source, bool state)
{ assert(alive[source]); ready = state; if (state) ready_ups++; return ESP_OK; }
void solar_os_input_composition_set_modifiers(solar_os_input_source_t source, uint8_t mods)
{ assert(alive[source]); (void)mods; }
uint8_t solar_os_input_composition_apply(uint8_t key) { return key; }
uint8_t solar_os_input_translate_hid_usage(uint16_t usage, uint8_t mods, bool caps)
{
    if (usage >= 4U && usage <= 29U) {
        if (mods & SOLAR_OS_INPUT_MOD_CTRL) return (uint8_t)(usage - 3U);
        return (uint8_t)((((mods & SOLAR_OS_INPUT_MOD_SHIFT) != 0) != caps ? 'A' : 'a') + usage - 4U);
    }
    if (usage == 0x52U) return SOLAR_OS_KEY_UP;
    return 0U;
}
esp_err_t solar_os_input_write_key(solar_os_input_source_t source, uint16_t physical,
    uint16_t usage, uint8_t key, uint8_t mods, solar_os_input_key_action_t action)
{
    assert(alive[source] && event_count < 256U);
    if (input_error != ESP_OK) return input_error;
    events[event_count++] = (solar_os_input_key_event_t){
        .source=source, .physical_key=physical, .usage=usage, .key=key, .modifiers=mods, .action=action,
    };
    return ESP_OK;
}
esp_err_t solar_os_input_write_char(solar_os_input_source_t source, char key)
{ assert(alive[source]); (void)key; return input_error; }
bool solar_os_expansion_find_i2c_bus(const char *name, solar_os_expansion_i2c_bus_t *bus, size_t *index)
{ (void)bus; (void)index; return strcmp(name, "i2c0") == 0 || strcmp(name, "i2c1") == 0; }
esp_err_t solar_os_bus_i2c_probe(const char *name, uint8_t address)
{ assert(name != NULL && address >= 8U && address <= 0x77U); return bus_error; }
esp_err_t solar_os_bus_i2c_read_reg(const char *name, uint8_t address, uint8_t reg, uint8_t *data, size_t len)
{
    assert(name != NULL && address >= 8U && len == 1U);
    if (bus_error != ESP_OK) return bus_error;
    if (reg == fail_read) return ESP_FAIL;
    if (reg == SOLAR_OS_TAB5_KEYBOARD_REG_COUNT) *data = (uint8_t)fifo_count;
    else if (reg == SOLAR_OS_TAB5_KEYBOARD_REG_EVENT) {
        read_events++;
        if (empty_race) { *data = 0xffU; fifo_count = 0; empty_race = false; }
        else {
            assert(fifo_count > 0U); *data = fifo[0];
            memmove(fifo, fifo + 1U, --fifo_count);
            if (refill) fifo[fifo_count++] = 0xa1U;
        }
    } else *data = regs[reg];
    return ESP_OK;
}
esp_err_t solar_os_bus_i2c_write_reg(const char *name, uint8_t address, uint8_t reg,
    const uint8_t *data, size_t len)
{
    assert(name != NULL && address >= 8U && len == 1U);
    if (bus_error != ESP_OK) return bus_error;
    if (reg == fail_write) return ESP_FAIL;
    regs[reg] = *data;
    if (reg == SOLAR_OS_TAB5_KEYBOARD_REG_COUNT && *data == 0U) fifo_count = 0;
    return ESP_OK;
}
static esp_err_t gpio_result(void) { return ++gpio_step == fail_gpio_step ? ESP_FAIL : ESP_OK; }
esp_err_t gpio_config(const gpio_config_t *config)
{ assert(config->mode == GPIO_MODE_INPUT && config->intr_type == GPIO_INTR_DISABLE); return gpio_result(); }
esp_err_t gpio_install_isr_service(int flags) { assert(flags == 0); return gpio_result(); }
esp_err_t gpio_isr_handler_add(gpio_num_t pin, void (*handler)(void *), void *arg)
{
    assert(pin == 1); esp_err_t err = gpio_result();
    if (err == ESP_OK) { gpio_handler = handler; gpio_context = arg; }
    return err;
}
esp_err_t gpio_isr_handler_remove(gpio_num_t pin)
{ assert(pin == 1); gpio_handler = NULL; gpio_context = NULL; return ESP_OK; }
esp_err_t gpio_set_intr_type(gpio_num_t pin, int type)
{ assert(pin == 1 && type == GPIO_INTR_NEGEDGE); return gpio_result(); }
esp_err_t gpio_intr_enable(gpio_num_t pin)
{ assert(pin == 1); esp_err_t err = gpio_result(); irq_enabled = err == ESP_OK; return err; }
esp_err_t gpio_intr_disable(gpio_num_t pin) { assert(pin == 1); irq_enabled = false; return ESP_OK; }
esp_err_t gpio_reset_pin(gpio_num_t pin) { assert(pin == 1); pin_resets++; return ESP_OK; }
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *wake)
{ assert(task != NULL && irq_enabled); irq_notifications++; *wake = pdTRUE; }
BaseType_t xTaskNotifyGive(TaskHandle_t task)
{ assert(task != NULL); notifications++; return pdPASS; }
BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t function, const char *name,
    uint32_t stack, void *arg, UBaseType_t priority, TaskHandle_t *task,
    BaseType_t core, solar_os_task_role_t role)
{
    assert(function == worker && name != NULL && stack == TAB5_STACK);
    (void)priority; (void)core; (void)role;
    if (task_failure) return pdFALSE;
    *task = arg; return pdPASS;
}
void solar_os_task_delete_internal(TaskHandle_t task) { assert(task == NULL); }
bool solar_os_task_wait_done(TaskHandle_t task, volatile bool *done, uint32_t timeout)
{
    assert(timeout == SOLAR_OS_TASK_STOP_WAIT_MS);
    if (wait_timeout) return false;
    worker(task); return *done;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t delay)
{
    assert(clear == pdTRUE); last_wait = delay;
    if (++worker_ticks >= worker_limit) ((tab5_device_t *)devices)->stop_requested = true;
    return 0U;
}
static void push(uint8_t event) { assert(fifo_count < 64U); fifo[fifo_count++] = event; }
static void poll_ok(tab5_device_t *dev)
{ assert(take_mutex()); assert(poll_once(dev) == ESP_OK); xSemaphoreGive(mutex); }
static solar_os_expansion_binding_t bindings[] = {
    {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target="i2c0"},
    {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value=0x6d},
    {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="irq", .value=1},
};
static void attach(unsigned count)
{
    regs[0x10] = 2U; regs[0] = 7U; gpio_step = 0;
    assert(solar_os_tab5_keyboard_attach("keyboard0", bindings, count) == ESP_OK);
    assert(regs[0x10] == 0U && regs[0] == 1U && fifo_count == 0U);
}
static void detach(void)
{
    assert(solar_os_tab5_keyboard_detach("keyboard0") == ESP_OK);
    assert(regs[0x10] == 2U && regs[0] == 7U && gpio_handler == NULL);
    assert(find_device("keyboard0") == NULL && opened == closed);
}
int main(void)
{
    uint16_t physical; bool pressed;
    for (unsigned row = 0; row < 5U; row++) for (unsigned col = 0; col < 14U; col++) {
        for (unsigned down = 0; down < 2U; down++) {
            assert(solar_os_tab5_keyboard_decode((uint8_t)(row * 16U + col + down * 128U), &physical, &pressed) == ESP_OK);
            assert(physical == 1U + row * 14U + col && pressed == (down != 0U));
        }
    }
    assert(solar_os_tab5_keyboard_decode(0xff, &physical, &pressed) == ESP_ERR_NOT_FOUND);
    assert(solar_os_tab5_keyboard_decode(0x0e, &physical, &pressed) == ESP_ERR_INVALID_RESPONSE);
    assert(solar_os_tab5_keyboard_decode(0x50, &physical, &pressed) == ESP_ERR_INVALID_RESPONSE);
    assert(solar_os_tab5_keyboard_attach(NULL, bindings, 2) == ESP_ERR_INVALID_ARG);
    assert(solar_os_tab5_keyboard_attach("keyboard0", bindings, 1) == ESP_ERR_INVALID_ARG);
    push(0xa1); attach(2); tab5_device_t *dev = find_device("keyboard0");
    assert(solar_os_tab5_keyboard_attach("keyboard0", bindings, 2) == ESP_ERR_INVALID_STATE);
    assert(solar_os_tab5_keyboard_attach("keyboard1", bindings, 2) == ESP_ERR_INVALID_STATE);
    solar_os_input_keymap_info_t info;
    assert(solar_os_input_keymap_info("keyboard0", &info) == ESP_OK && info.key_count == 70U && info.stride == 14U);
    push(0xa1); push(0x21); poll_ok(dev);
    assert(event_count == 2U && events[0].physical_key == 30U && events[0].usage == 0x14U);
    assert(events[0].key == 'q' && events[1].action == SOLAR_OS_INPUT_KEY_RELEASE);
    /* Held Aa, letter, release Aa first: physical/HID identity stays stable. */
    push(0xb1); push(0xb2); push(0x31); push(0x32); poll_ok(dev);
    assert(events[3].key == 'A' && events[3].modifiers == SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    assert(events[5].physical_key == events[3].physical_key &&
           events[5].usage == events[3].usage && events[5].modifiers == 0U);
    /* Printed punctuation and Sym layer; every position has stable identity. */
    push(0x91); push(0x11); poll_ok(dev); assert(events[event_count-1U].key == '!');
    push(0xb0); push(0x91); push(0x11); push(0x30); poll_ok(dev);
    assert(events[event_count-2U].key == '?');
    push(0xc0); push(0xb2); poll_ok(dev);
    assert(events[event_count-1U].modifiers == SOLAR_OS_INPUT_MOD_LEFT_CTRL);
    solar_os_input_keymap_t *map = malloc(sizeof(*map)); assert(map != NULL);
    solar_os_tab5_keyboard_map(map); assert(solar_os_input_keymap_validate(map) == ESP_OK);
    map->keys[0][30].usage = 0x04U;
    push(0xa1); assert(solar_os_input_keymap_set("keyboard0", map) == ESP_OK && fifo_count == 0U);
    push(0xa1); push(0x21); poll_ok(dev); assert(events[event_count-1U].key == 'a');
    assert(solar_os_input_keymap_reset("keyboard0") == ESP_OK);
    free(map);
    push(0xa1); empty_race = true; poll_ok(dev);
    /* Continuous refill is bounded to 32 reads, not an unbounded drain. */
    refill = true; push(0xa1); unsigned before = read_events; poll_ok(dev);
    assert(read_events - before == 32U); refill = false;
    detach();
    /* Saturation, malformed events, read/ack failures publish no staged keys. */
    attach(2); dev = find_device("keyboard0"); unsigned prior = event_count;
    for (unsigned i = 0; i < 32U; i++) push(0xa1);
    assert(poll_once(dev) == ESP_ERR_INVALID_RESPONSE && event_count == prior);
    assert(solar_os_tab5_keyboard_clear(dev->bus, dev->address) == ESP_OK);
    push(0xa1); push(0xfe);
    assert(poll_once(dev) == ESP_ERR_INVALID_RESPONSE && event_count == prior);
    assert(solar_os_tab5_keyboard_clear(dev->bus, dev->address) == ESP_OK);
    push(0xa1); fail_write = 1;
    assert(poll_once(dev) == ESP_FAIL && event_count == prior); fail_write = -1;
    push(0xa1); fail_read = 0x20;
    assert(poll_once(dev) == ESP_FAIL && event_count == prior); fail_read = -1;
    bus_error = ESP_FAIL; ready = true; worker_limit = 1; worker_ticks = 0;
    before = released; worker(dev);
    assert(dev->recovering && released > before && !ready && last_wait == TAB5_RETRY_MS);
    bus_error = ESP_OK; dev->stop_requested = false; dev->worker_done = false;
    worker_limit = 1; worker_ticks = 0; worker(dev);
    assert(!dev->recovering && ready_ups == 1U && fifo_count == 0U && last_wait == TAB5_POLL_MS);
    detach();
    /* Optional IRQ wakes a valid task and is removed before timeout/stop. */
    attach(3); dev = find_device("keyboard0");
    assert(gpio_handler != NULL && irq_enabled); gpio_handler(gpio_context);
    assert(irq_notifications == 1U);
    wait_timeout = true;
    assert(solar_os_tab5_keyboard_detach("keyboard0") == ESP_ERR_TIMEOUT);
    assert(find_device("keyboard0") != NULL && gpio_handler == NULL && !irq_enabled);
    wait_timeout = false; detach();
    /* Failed restore keeps ownership and makes a subsequent detach safe. */
    attach(2); fail_write = 0x10;
    assert(solar_os_tab5_keyboard_detach("keyboard0") == ESP_FAIL);
    assert(find_device("keyboard0") != NULL); fail_write = -1; detach();
    for (int step = 1; step <= 5; step++) {
        regs[0x10] = 2; regs[0] = 7; gpio_step = 0; fail_gpio_step = step;
        assert(solar_os_tab5_keyboard_attach("keyboard0", bindings, 3) == ESP_FAIL);
        assert(gpio_handler == NULL && regs[0x10] == 2 && regs[0] == 7 && opened == closed);
    }
    fail_gpio_step = 0;
    for (unsigned allocation = 1; allocation <= 2; allocation++) {
        fail_allocation = allocations + allocation;
        assert(solar_os_tab5_keyboard_attach("keyboard0", bindings, 2) == ESP_ERR_NO_MEM);
        assert(opened == closed && find_device("keyboard0") == NULL);
    }
    fail_allocation = 0; source_failure = true;
    assert(solar_os_tab5_keyboard_attach("keyboard0", bindings, 2) == ESP_ERR_NO_MEM);
    source_failure = false; task_failure = true;
    assert(solar_os_tab5_keyboard_attach("keyboard0", bindings, 3) == ESP_ERR_NO_MEM);
    assert(opened == closed && gpio_handler == NULL && !irq_enabled);
    task_failure = false; bindings[1].value = 0x6e; attach(2); detach();
    bindings[1].value = 0x6d;
    /* Input queue exhaustion and mode changes discard unreliable held state. */
    attach(2); dev = find_device("keyboard0"); input_error = ESP_ERR_NO_MEM;
    push(0xa1); worker_limit = 1; worker_ticks = 0; before = released;
    worker(dev); assert(dev->recovering && released > before && !ready);
    input_error = ESP_OK; detach();
    attach(2); dev = find_device("keyboard0"); regs[0x10] = 1;
    assert(poll_once(dev) == ESP_ERR_INVALID_STATE);
    detach();
    /* Distinct addresses/buses can coexist, with independent input sources. */
    attach(2); bindings[1].value = 0x6e;
    assert(solar_os_tab5_keyboard_attach("keyboard1", bindings, 2) == ESP_OK);
    assert(find_device("keyboard1")->source != find_device("keyboard0")->source);
    assert(solar_os_tab5_keyboard_detach("keyboard1") == ESP_OK);
    bindings[1].value = 0x6d; detach();
    puts("tab5_keyboard_test: ok");
    return 0;
}
