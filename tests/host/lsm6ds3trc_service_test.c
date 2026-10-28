#include <assert.h>
#include <string.h>
#include "solar_os_lsm6ds3trc.h"
#include "solar_os_imu.h"
#include "solar_os_buses.h"
#include "freertos/semphr.h"
#include "../../src/services/solar_os_lsm6ds3trc.c"

static uint8_t registers[256];
static int64_t now;
static solar_os_imu_registration_t provider;
static esp_err_t register_result, unregister_result;
static unsigned released;
static int irq_level;
static unsigned source_opens, source_closes, event_count;
static esp_err_t source_result;
static bool task_fail, wait_fail;
static TaskFunction_t worker_fn;
static void *worker_context;
static solar_os_input_gesture_event_t events[32];

esp_err_t gpio_config(const gpio_config_t *config) {
    assert(config->pin_bit_mask == (1ULL << 7) && config->mode == GPIO_MODE_INPUT);
    return ESP_OK;
}
int gpio_get_level(gpio_num_t pin) { assert(pin == 7); return irq_level; }
esp_err_t solar_os_input_source_open_typed(const char *name,
    solar_os_input_source_class_t kind, uint32_t caps, bool ready,
    solar_os_input_source_t *source) {
    assert(!strcmp(name, "imu0") && kind == SOLAR_OS_INPUT_SOURCE_GESTURE);
    assert(caps == SOLAR_OS_INPUT_CAP_GESTURE_EVENTS && ready);
    if (source_result != ESP_OK) return source_result;
    source_opens++; *source = 1; return ESP_OK;
}
void solar_os_input_source_close(solar_os_input_source_t source) {
    assert(source == 1);
    assert(!((lsm6ds3trc_device_t *)provider.ctx)->worker_task);
    source_closes++;
}
esp_err_t solar_os_input_source_set_gestures(solar_os_input_source_t source, uint32_t mask) {
    assert(source == 1 && __builtin_popcount(mask) == 5);
    return ESP_OK;
}
esp_err_t solar_os_input_write_gesture(solar_os_input_source_t source,
    const solar_os_input_gesture_event_t *event) {
    assert(source == 1 && event_count < 32);
    events[event_count++] = *event; return ESP_OK;
}
BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t fn, const char *name,
    uint32_t stack, void *ctx, UBaseType_t priority, TaskHandle_t *handle,
    BaseType_t core, solar_os_task_role_t role) {
    assert(!strcmp(name,"imu0") && stack == 3072 && priority == 1);
    assert(core == tskNO_AFFINITY && role == SOLAR_OS_TASK_ROLE_BACKGROUND);
    if (task_fail) return pdFAIL;
    worker_fn=fn; worker_context=ctx; *handle=(void *)1; return pdPASS;
}
BaseType_t xTaskNotifyGive(TaskHandle_t task) { assert(task == (void *)1); return pdPASS; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait) { (void)clear;(void)wait;return 0; }
void solar_os_task_delete_internal(TaskHandle_t task) { assert(task == NULL); }
bool solar_os_task_wait_done(TaskHandle_t task, volatile bool *done, uint32_t timeout) {
    assert(task == (void *)1 && timeout == 2000);
    if (wait_fail) return false;
    worker_fn(worker_context);
    assert(*done);return true;
}

static void poll_events(int irq, uint8_t wake, uint8_t tap, uint8_t function, unsigned ms) {
    now += ms * 1000ULL; irq_level = irq;
    registers[0x1b]=wake;registers[0x1c]=tap;registers[0x53]=function;
    poll_gestures(worker_context);
}

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t length = strlen(src);
    if (size) { size_t n = length < size - 1 ? length : size - 1; memcpy(dst, src, n); dst[n] = 0; }
    return length;
}
void vSemaphoreDelete(SemaphoreHandle_t mutex) { assert(mutex); released++; }
void vTaskDelay(TickType_t ticks) { now += (int64_t)ticks * 1000; }
int64_t esp_timer_get_time(void) { return now; }
bool solar_os_expansion_find_i2c_bus(const char *name, solar_os_expansion_i2c_bus_t *bus, size_t *index) {
    (void)bus; (void)index; return !strcmp(name, "i2c0");
}
esp_err_t solar_os_bus_i2c_read_reg(const char *bus, uint8_t addr, uint8_t reg, uint8_t *data, size_t size) {
    assert(!strcmp(bus, "i2c0") && addr == 0x6a);
    if (reg == 0x12) registers[reg] &= ~1U;
    memcpy(data, registers + reg, size); return ESP_OK;
}
esp_err_t solar_os_bus_i2c_write_reg(const char *bus, uint8_t addr, uint8_t reg, const uint8_t *data, size_t size) {
    assert(!strcmp(bus, "i2c0") && addr == 0x6a && size == 1);
    registers[reg] = *data; return ESP_OK;
}
esp_err_t solar_os_imu_register(const solar_os_imu_registration_t *registration) {
    assert(!strcmp(registration->driver, "lsm6ds3tr-c"));
    assert(registration->capabilities == (SOLAR_OS_IMU_CAP_ACCELERATION | SOLAR_OS_IMU_CAP_ANGULAR_VELOCITY));
    provider = *registration; return register_result;
}
esp_err_t solar_os_imu_unregister(const char *name) {
    assert(!strcmp(name, "imu0")); return unregister_result;
}
int main(void) {
    const solar_os_expansion_binding_t bindings[] = {
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target="i2c0"},
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value=0x6a},
    };
    registers[0x0f] = 0x6a;
    register_result = ESP_FAIL;
    assert(solar_os_lsm6ds3trc_attach("imu0", bindings, 2) == ESP_FAIL);
    assert(released == 1 && registers[0x10] == 0 && registers[0x11] == 0);
    register_result = ESP_OK;
    assert(solar_os_lsm6ds3trc_attach("imu0", bindings, 2) == ESP_OK);
    assert(solar_os_lsm6ds3trc_attach("imu0", bindings, 2) == ESP_ERR_INVALID_STATE);
    solar_os_imu_sample_t sample = {.timestamp_us=123}, before = sample;
    assert(provider.ops->read_sample(provider.ctx, 10, &sample) == ESP_ERR_TIMEOUT);
    assert(now == 10000 && !memcmp(&sample, &before, sizeof(sample)));
    registers[0x1e] = 3;
    assert(provider.ops->read_sample(provider.ctx, 10, &sample) == ESP_OK);
    assert(sample.valid == provider.capabilities && sample.timestamp_us == 10000);
    unregister_result = ESP_FAIL;
    assert(solar_os_lsm6ds3trc_detach("imu0") == ESP_FAIL && registers[0x10] == 0x40);
    unregister_result = ESP_OK;
    assert(solar_os_lsm6ds3trc_detach("imu0") == ESP_OK && released == 2);
    assert(registers[0x10] == 0 && registers[0x11] == 0);
    const solar_os_expansion_binding_t gesture_bindings[] = {
        bindings[0], bindings[1],
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO,.role="irq",.value=7},
        {.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER,.role="rotation",.value=1},
    };
    source_result = ESP_FAIL;
    assert(solar_os_lsm6ds3trc_attach("imu0", gesture_bindings, 4) == ESP_FAIL);
    assert(!registers[0x10] && !registers[0x5e] && !source_opens);
    source_result = ESP_OK; task_fail = true;
    assert(solar_os_lsm6ds3trc_attach("imu0", gesture_bindings, 4) == ESP_ERR_NO_MEM);
    assert(source_opens == source_closes && !registers[0x10]);
    task_fail = false; register_result = ESP_FAIL;
    assert(solar_os_lsm6ds3trc_attach("imu0", gesture_bindings, 4) == ESP_FAIL);
    assert(source_opens == source_closes && !registers[0x10]);
    register_result = ESP_OK;
    assert(solar_os_lsm6ds3trc_attach("imu0", gesture_bindings, 4) == ESP_OK);
    assert(registers[0x10] == 0x60 && registers[0x11] == 0x40);
    poll_events(1,0x28,0,0x20,0);
    assert(event_count == 3 && events[0].gesture == SOLAR_OS_INPUT_GESTURE_TILT);
    assert(events[1].gesture == SOLAR_OS_INPUT_GESTURE_WAKE);
    assert(events[2].gesture == SOLAR_OS_INPUT_GESTURE_FREE_FALL);
    assert(events[2].raw == 0x200028 && events[2].direction == SOLAR_OS_INPUT_GESTURE_DIRECTION_NONE);
    poll_events(1,0x28,0,0x20,10);assert(event_count == 3);
    poll_events(0,0,0,0,10);
    poll_events(1,0,0x60,0,10);assert(event_count == 3);
    poll_events(1,0,0x60,0,100);assert(event_count == 3);
    poll_events(0,0,0,0,549);assert(event_count == 3);
    poll_events(0,0,0,0,1);assert(event_count == 4);
    assert(events[3].gesture == SOLAR_OS_INPUT_GESTURE_TAP && events[3].value == 1);
    poll_events(1,0,0x60,0,10);
    poll_events(0,0,0,0,100);
    poll_events(1,0,0x50,0,100);assert(event_count == 5);
    assert(events[4].gesture == SOLAR_OS_INPUT_GESTURE_DOUBLE_TAP && events[4].value == 2);
    assert(events[4].flags == SOLAR_OS_INPUT_GESTURE_FLAG_DOUBLE);
    poll_events(1,0,0x50,0,10);assert(event_count == 5);
    poll_events(0,0,0,0,700);assert(event_count == 5);
    const uint8_t positions[] = {0x01, 0x08, 0x02, 0x04};
    const solar_os_input_gesture_direction_t directions[] = {
        SOLAR_OS_INPUT_GESTURE_DIRECTION_NORTH, SOLAR_OS_INPUT_GESTURE_DIRECTION_EAST,
        SOLAR_OS_INPUT_GESTURE_DIRECTION_SOUTH, SOLAR_OS_INPUT_GESTURE_DIRECTION_WEST,
    };
    for (unsigned i = 0; i < 4; i++) {
        registers[0x1d] = positions[i];
        poll_events(0,0,0,0,50);
        assert(event_count == 6 + i);
        assert(events[5 + i].gesture == SOLAR_OS_INPUT_GESTURE_TILT);
        assert(events[5 + i].direction == directions[i]);
        assert(events[5 + i].value == (int)(i * 90U));
        assert(events[5 + i].raw == (uint32_t)positions[i] << 24U);
        poll_events(0,0,0,0,50);
        assert(event_count == 6 + i);
    }
    for (unsigned i = 0; i < 3; i++) {
        registers[0x1d] = (uint8_t[]){0x50, 0x60, 0x43}[i];
        poll_events(1,0,0,0,10);
        assert(event_count == 9);
    }
    unregister_result = ESP_FAIL;
    assert(solar_os_lsm6ds3trc_detach("imu0") == ESP_FAIL);
    assert(source_opens != source_closes && registers[0x10] == 0x60);
    unregister_result = ESP_OK; wait_fail = true;
    assert(solar_os_lsm6ds3trc_detach("imu0") == ESP_ERR_TIMEOUT);
    assert(source_opens != source_closes && registers[0x10] == 0x60);
    wait_fail = false;
    assert(solar_os_lsm6ds3trc_detach("imu0") == ESP_OK);
    assert(source_opens == source_closes && !registers[0x10] && !registers[0x5e]);
    return 0;
}
