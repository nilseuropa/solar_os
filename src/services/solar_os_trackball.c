#include "solar_os_trackball.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_input.h"
#include "solar_os_input_composition.h"
#include "solar_os_memory.h"

#define TRACKBALL_MAX 4U
#define TRACKBALL_EDGES 64U
typedef struct trackball trackball_t;
typedef struct {
    int pin;
    bool configured, installed;
} channel_t;
typedef struct {
    uint32_t time_ms;
    int8_t x, y;
    uint8_t modifiers;
} edge_t;
struct trackball {
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    solar_os_input_source_t source;
    channel_t channels[4];
    edge_t edges[TRACKBALL_EDGES];
    size_t head, count, slot;
    bool active;
};
static trackball_t *devices[TRACKBALL_MAX];
static struct {
    trackball_t *device;
    uint32_t generation;
} irq_slots[TRACKBALL_MAX];
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static portMUX_TYPE edge_lock = portMUX_INITIALIZER_UNLOCKED;

static void take_mutex(void)
{
    portENTER_CRITICAL(&edge_lock);
    if (!mutex)
        mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    portEXIT_CRITICAL(&edge_lock);
    xSemaphoreTake(mutex, portMAX_DELAY);
}

static void trackball_edge(void *arg)
{
    /* Encode a generation and channel rather than borrowing allocated memory:
     * a callback already in flight may finish after handler removal on another
     * core, or after this slot has been reattached. */
    const uintptr_t token = (uintptr_t)arg;
    const size_t slot = (token & 15U) / 4U;
    const unsigned direction = token & 3U;
    const edge_t edge = {.time_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
                         .x = direction == 2   ? -1
                              : direction == 3 ? 1
                                               : 0,
                         .y = direction == 0   ? -1
                              : direction == 1 ? 1
                                               : 0,
                         .modifiers = solar_os_input_composition_modifiers()};
    portENTER_CRITICAL_ISR(&edge_lock);
    trackball_t *device = irq_slots[slot].device;
    if (device && irq_slots[slot].generation == token >> 4 && device->active &&
        device->count < TRACKBALL_EDGES) {
        device->edges[(device->head + device->count++) % TRACKBALL_EDGES] = edge;
    }
    portEXIT_CRITICAL_ISR(&edge_lock);
}

static void clear_device(trackball_t *device)
{
    portENTER_CRITICAL(&edge_lock);
    device->active = false;
    irq_slots[device->slot].device = NULL;
    portEXIT_CRITICAL(&edge_lock);
    for (size_t i = 0; i < 4U; i++) {
        channel_t *channel = &device->channels[i];
        if (channel->installed) {
            (void)gpio_set_intr_type(channel->pin, GPIO_INTR_DISABLE);
            (void)gpio_isr_handler_remove(channel->pin);
        }
        if (channel->configured)
            (void)gpio_reset_pin(channel->pin);
    }
    if (device->source)
        solar_os_input_source_close(device->source);
    solar_os_memory_free(device);
}

esp_err_t solar_os_trackball_attach(const char *name, const solar_os_expansion_binding_t *bindings,
                                    size_t count)
{
    if (!name || !name[0] || strlen(name) >= SOLAR_OS_INPUT_SOURCE_NAME_MAX || !bindings)
        return ESP_ERR_INVALID_ARG;
    int pins[4] = {-1, -1, -1, -1};
    int pull = 1;
    bool have_pull = false;
    static const char *const roles[] = {"up", "down", "left", "right"};
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        size_t direction = 0;
        while (direction < 4U && strcmp(b->role, roles[direction]))
            direction++;
        if (direction < 4U && b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO && pins[direction] < 0 &&
            GPIO_IS_VALID_GPIO(b->value))
            pins[direction] = b->value;
        else if (!strcmp(b->role, "pull") && b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                 !have_pull && b->value >= 0 && b->value <= 2) {
            pull = b->value;
            have_pull = true;
        } else
            return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < 4U; i++) {
        if (pins[i] < 0)
            return ESP_ERR_INVALID_ARG;
        for (size_t j = 0; j < i; j++)
            if (pins[i] == pins[j])
                return ESP_ERR_INVALID_ARG;
    }
    take_mutex();
    size_t slot = TRACKBALL_MAX;
    for (size_t i = 0; i < TRACKBALL_MAX; i++) {
        if (devices[i] && !strcmp(devices[i]->name, name)) {
            xSemaphoreGive(mutex);
            return ESP_ERR_INVALID_STATE;
        }
        if (!devices[i] && slot == TRACKBALL_MAX)
            slot = i;
    }
    trackball_t *device =
        slot < TRACKBALL_MAX
            ? solar_os_memory_calloc(1, sizeof(*device), SOLAR_OS_MEMORY_INTERNAL_CRITICAL,
                                     "trackball.irq")
            : NULL;
    if (!device) {
        xSemaphoreGive(mutex);
        return ESP_ERR_NO_MEM;
    }
    strlcpy(device->name, name, sizeof(device->name));
    device->slot = slot;
    portENTER_CRITICAL(&edge_lock);
    irq_slots[slot].generation = (irq_slots[slot].generation + 1U) & 0x0fffffffU;
    irq_slots[slot].device = device;
    const uintptr_t token = (uintptr_t)irq_slots[slot].generation << 4;
    portEXIT_CRITICAL(&edge_lock);
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_ERR_INVALID_STATE)
        err = ESP_OK;
    for (size_t i = 0; i < 4U && err == ESP_OK; i++) {
        channel_t *channel = &device->channels[i];
        *channel = (channel_t){.pin = pins[i]};
        /* Expansion has already claimed these pins. Board attachments may
         * use fixed internal pins, outside the runtime GPIO service policy. */
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << channel->pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = pull == 1 ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = pull == 2 ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&config);
        if (err != ESP_OK)
            break;
        channel->configured = true;
        err = gpio_isr_handler_add(channel->pin, trackball_edge, (void *)(token | (slot * 4U + i)));
        if (err != ESP_OK)
            break;
        channel->installed = true;
        err = gpio_set_intr_type(channel->pin, GPIO_INTR_ANYEDGE);
    }
    if (err == ESP_OK)
        err = solar_os_input_source_open_typed(name, SOLAR_OS_INPUT_SOURCE_MOUSE,
                                               SOLAR_OS_INPUT_CAP_POINTER_RELATIVE, true,
                                               &device->source);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&edge_lock);
        device->active = true;
        portEXIT_CRITICAL(&edge_lock);
        devices[slot] = device;
    } else
        clear_device(device);
    xSemaphoreGive(mutex);
    return err;
}

esp_err_t solar_os_trackball_detach(const char *name)
{
    if (!name)
        return ESP_ERR_INVALID_ARG;
    take_mutex();
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < TRACKBALL_MAX; i++) {
        if (devices[i] && !strcmp(devices[i]->name, name)) {
            clear_device(devices[i]);
            devices[i] = NULL;
            err = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(mutex);
    return err;
}

bool solar_os_trackball_active(void)
{
    bool active = false;
    take_mutex();
    for (size_t i = 0; i < TRACKBALL_MAX; i++)
        active |= devices[i] != NULL;
    xSemaphoreGive(mutex);
    return active;
}

void solar_os_trackball_poll(void)
{
    take_mutex();
    for (size_t i = 0; i < TRACKBALL_MAX; i++) {
        trackball_t *device = devices[i];
        if (!device)
            continue;
        /* Preserve timestamps across delayed polling; leave backpressured
         * edges pending. No task or stack is allocated per attachment. */
        for (size_t n = 0; n < TRACKBALL_EDGES; n++) {
            portENTER_CRITICAL(&edge_lock);
            const bool pending = device->count != 0;
            const edge_t edge = pending ? device->edges[device->head] : (edge_t){0};
            portEXIT_CRITICAL(&edge_lock);
            if (!pending)
                break;
            const solar_os_input_pointer_sample_t sample = {
                .event = {.mode = SOLAR_OS_INPUT_POINTER_RELATIVE,
                          .action = SOLAR_OS_INPUT_POINTER_MOVE,
                          .delta_x = edge.x,
                          .delta_y = edge.y},
                .time_ms = edge.time_ms,
                .modifiers = edge.modifiers,
                .modifiers_valid = true,
            };
            if (solar_os_input_write_pointer_sample(device->source, &sample) != ESP_OK)
                break;
            portENTER_CRITICAL(&edge_lock);
            device->head = (device->head + 1U) % TRACKBALL_EDGES;
            device->count--;
            portEXIT_CRITICAL(&edge_lock);
        }
    }
    xSemaphoreGive(mutex);
}
