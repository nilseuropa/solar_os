#include "solar_os_expansion.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_board.h"
#include "solar_os_buses.h"
#include "solar_os_config.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_memory.h"
#include "solar_os_pins.h"
#include "solar_os_resources.h"
#include "solar_os_stream.h"
#include "soc/soc_caps.h"

static const solar_os_expansion_driver_t manual_expansion_driver = {
    .name = "manual",
    .summary = "custom resource map",
    .category = SOLAR_OS_EXPANSION_CATEGORY_UTILITY,
    .allow_unlisted_bindings = true,
};

#define SOLAR_OS_DECLARE_EXPANSION_DRIVER(symbol) \
    extern const solar_os_expansion_driver_t symbol;
SOLAR_OS_EXPANSION_DRIVER_SYMBOLS(SOLAR_OS_DECLARE_EXPANSION_DRIVER)
#undef SOLAR_OS_DECLARE_EXPANSION_DRIVER

#define SOLAR_OS_EXPANSION_DRIVER_POINTER(symbol) &symbol,
static const solar_os_expansion_driver_t *const expansion_drivers[] = {
    &manual_expansion_driver,
    SOLAR_OS_EXPANSION_DRIVER_SYMBOLS(SOLAR_OS_EXPANSION_DRIVER_POINTER)
};
#undef SOLAR_OS_EXPANSION_DRIVER_POINTER

typedef enum {
    EXPANSION_SLOT_ATTACHING,
    EXPANSION_SLOT_ACTIVE,
    EXPANSION_SLOT_DETACHING,
} expansion_slot_state_t;

typedef struct expansion_device_node {
    solar_os_expansion_device_t device;
    expansion_slot_state_t state;
    struct expansion_device_node *next;
} expansion_device_node_t;

static expansion_device_node_t *devices;
static SemaphoreHandle_t devices_mutex;
static StaticSemaphore_t devices_mutex_storage;

#define EXPANSION_DEFAULT_RETRY_INITIAL_MS 500U
#define EXPANSION_DEFAULT_RETRY_MAX_MS 4000U
#define EXPANSION_DEFAULT_RETRY_LIMIT 8U

static bool default_retry_pending;
static bool default_retry_scheduled;
static uint8_t default_retry_attempts;
static uint32_t default_retry_due_ms;
static uint32_t default_retry_delay_ms;

#if SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT > 0
static const solar_os_expansion_default_device_t board_default_devices[] =
    SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICES;

_Static_assert(sizeof(board_default_devices) / sizeof(board_default_devices[0]) ==
                   SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT,
               "board expansion-device count does not match its definitions");
#endif

static esp_err_t expansion_attach(const char *driver,
                                  const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count,
                                  solar_os_expansion_origin_t origin,
                                  bool autostart,
                                  bool detachable);

static bool mask_contains(uint64_t mask, int pin)
{
    return pin >= 0 && pin < 64 && (mask & (1ULL << (uint32_t)pin)) != 0;
}

static bool device_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0' || strncmp(name, "bus:", 4) == 0) {
        return false;
    }
    return strnlen(name, SOLAR_OS_EXPANSION_DEVICE_NAME_MAX) < SOLAR_OS_EXPANSION_DEVICE_NAME_MAX;
}

static esp_err_t ensure_devices_mutex(void)
{
    if (devices_mutex == NULL) {
        devices_mutex = xSemaphoreCreateMutexStatic(&devices_mutex_storage);
    }
    return devices_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool devices_lock_take(void)
{
    return ensure_devices_mutex() == ESP_OK &&
        xSemaphoreTake(devices_mutex, portMAX_DELAY) == pdTRUE;
}

static void devices_lock_give(void)
{
    xSemaphoreGive(devices_mutex);
}

static expansion_device_node_t *find_device_locked(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (expansion_device_node_t *node = devices; node != NULL; node = node->next) {
        if (strcmp(node->device.name, name) == 0) {
            return node;
        }
    }
    return NULL;
}

static void append_device_locked(expansion_device_node_t *node)
{
    expansion_device_node_t **link = &devices;
    while (*link != NULL) {
        link = &(*link)->next;
    }
    *link = node;
}

static bool remove_device_locked(expansion_device_node_t *node)
{
    expansion_device_node_t **link = &devices;
    while (*link != NULL) {
        if (*link == node) {
            *link = node->next;
            node->next = NULL;
            return true;
        }
        link = &(*link)->next;
    }
    return false;
}

static void release_device_reservation(expansion_device_node_t *node)
{
    bool removed = false;
    if (devices_lock_take()) {
        if (node->state == EXPANSION_SLOT_ATTACHING) {
            removed = remove_device_locked(node);
        }
        devices_lock_give();
    }
    if (removed) {
        solar_os_memory_free(node);
    }
}

static const solar_os_expansion_driver_t *find_driver(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(expansion_drivers) / sizeof(expansion_drivers[0]); i++) {
        if (strcmp(expansion_drivers[i]->name, name) == 0) {
            return expansion_drivers[i];
        }
    }
    return NULL;
}

static bool binding_matches_spec(const solar_os_expansion_binding_t *binding,
                                 const solar_os_expansion_binding_spec_t *spec)
{
    if (binding == NULL || spec == NULL || binding->kind != spec->kind) {
        return false;
    }
    if (spec->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
        spec->role == NULL) {
        return binding->role[0] == '\0';
    }
    return spec->role == NULL || strcmp(binding->role, spec->role) == 0;
}

static const char *binding_key(const solar_os_expansion_binding_t *binding)
{
    if (binding == NULL) {
        return "resource";
    }
    switch (binding->kind) {
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
    case SOLAR_OS_EXPANSION_BINDING_GPIO_LINE:
    case SOLAR_OS_EXPANSION_BINDING_ADC:
    case SOLAR_OS_EXPANSION_BINDING_PWM:
        return binding->role[0] != '\0' ? binding->role :
            solar_os_expansion_binding_kind_name(binding->kind);
    case SOLAR_OS_EXPANSION_BINDING_I2S_PORT:
        return "i2s";
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
        return "i2c";
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
        return binding->role[0] != '\0' ? binding->role : "addr";
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
        return "spi";
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
        return "cs";
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT:
        return "uart";
    case SOLAR_OS_EXPANSION_BINDING_PS2_BUS:
        return "ps2";
    case SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM:
        return binding->role[0] != '\0' ? binding->role : "stream";
    case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
        return binding->role[0] != '\0' ? binding->role : "parameter";
    default:
        return "resource";
    }
}

static void set_binding_validation(solar_os_expansion_binding_validation_t *validation,
                                   solar_os_expansion_binding_validation_reason_t reason,
                                   const char *key)
{
    if (validation == NULL) {
        return;
    }
    validation->reason = reason;
    strlcpy(validation->key, key != NULL ? key : "resource", sizeof(validation->key));
}

static bool pin_is_expansion_gpio(int pin)
{
    return solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_GPIO) &&
        solar_os_pin_is_direct_gpio(pin);
}

static bool pin_is_expansion_adc(int pin)
{
    return solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_ADC) &&
        mask_contains(SOLAR_OS_BOARD_EXPANSION_ADC_MASK, pin);
}

static bool pin_is_expansion_pwm(int pin)
{
    return solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_PWM) &&
        mask_contains(SOLAR_OS_BOARD_EXPANSION_PWM_MASK, pin);
}

static bool first_i2c_binding(const solar_os_expansion_binding_t *bindings,
                              size_t binding_count,
                              char *target,
                              size_t target_len,
                              size_t *bus_index)
{
    for (size_t i = 0; i < binding_count; i++) {
        if (bindings[i].kind != SOLAR_OS_EXPANSION_BINDING_I2C_BUS) {
            continue;
        }
        size_t found_index = 0;
        if (!solar_os_expansion_find_i2c_bus(bindings[i].target, NULL, &found_index)) {
            return false;
        }
        if (target != NULL && target_len > 0) {
            strlcpy(target, bindings[i].target, target_len);
        }
        if (bus_index != NULL) {
            *bus_index = found_index;
        }
        return true;
    }
    return false;
}

static esp_err_t append_claim(solar_os_resource_request_t *requests,
                              size_t *request_count,
                              solar_os_resource_kind_t kind,
                              int primary,
                              int secondary,
                              const char *label)
{
    if (requests == NULL || request_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*request_count >= SOLAR_OS_RESOURCE_BUNDLE_MAX) {
        return ESP_ERR_NO_MEM;
    }
    requests[(*request_count)++] = (solar_os_resource_request_t) {
        .kind = kind,
        .primary = primary,
        .secondary = secondary,
        .label = label,
    };
    return ESP_OK;
}

static esp_err_t append_binding_claims(const solar_os_expansion_binding_t *binding,
                                       const solar_os_expansion_binding_t *bindings,
                                       size_t binding_count,
                                       solar_os_resource_request_t *requests,
                                       size_t *request_count)
{
    switch (binding->kind) {
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
        return append_claim(requests,
                            request_count,
                            SOLAR_OS_RESOURCE_GPIO_PIN,
                            binding->value,
                            -1,
                            binding->role);
    case SOLAR_OS_EXPANSION_BINDING_GPIO_LINE: {
        if (binding->target[0] == '\0') {
            return append_claim(requests,
                                request_count,
                                SOLAR_OS_RESOURCE_GPIO_PIN,
                                binding->value,
                                -1,
                                binding->role);
        }
        solar_os_gpio_controller_info_t controller;
        if (!solar_os_gpio_controller_find(binding->target, &controller) ||
            binding->value < 0 || binding->value >= controller.line_count) {
            return ESP_ERR_INVALID_ARG;
        }
        return append_claim(requests,
                            request_count,
                            SOLAR_OS_RESOURCE_GPIO_LINE,
                            controller.id,
                            binding->value,
                            binding->role);
    }
    case SOLAR_OS_EXPANSION_BINDING_ADC:
        ESP_RETURN_ON_ERROR(append_claim(requests,
                                         request_count,
                                         SOLAR_OS_RESOURCE_ADC_PIN,
                                         binding->value,
                                         -1,
                                         binding->role),
                            "expansion",
                            "append adc claim failed");
        return append_claim(requests,
                            request_count,
                            SOLAR_OS_RESOURCE_GPIO_PIN,
                            binding->value,
                            -1,
                            "adc");
    case SOLAR_OS_EXPANSION_BINDING_PWM:
        ESP_RETURN_ON_ERROR(append_claim(requests,
                                         request_count,
                                         SOLAR_OS_RESOURCE_PWM_PIN,
                                         binding->value,
                                         -1,
                                         binding->role),
                            "expansion",
                            "append pwm claim failed");
        return append_claim(requests,
                            request_count,
                            SOLAR_OS_RESOURCE_GPIO_PIN,
                            binding->value,
                            -1,
                            "pwm");
    case SOLAR_OS_EXPANSION_BINDING_I2S_PORT:
        return append_claim(requests,
                            request_count,
                            SOLAR_OS_RESOURCE_I2S_PORT,
                            binding->value,
                            -1,
                            "i2s");
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
        ESP_RETURN_ON_ERROR(append_claim(requests,
                                         request_count,
                                         SOLAR_OS_RESOURCE_SPI_CS,
                                         binding->value,
                                         -1,
                                         binding->target),
                            "expansion",
                            "append spi cs claim failed");
        return append_claim(requests,
                            request_count,
                            SOLAR_OS_RESOURCE_GPIO_PIN,
                            binding->value,
                            -1,
                            "spi-cs");
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS: {
        char target[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
        size_t bus_index = 0;
        if (binding->target[0] != '\0') {
            if (!solar_os_expansion_find_i2c_bus(binding->target, NULL, &bus_index)) {
                return ESP_ERR_INVALID_ARG;
            }
        } else if (!first_i2c_binding(bindings, binding_count, target, sizeof(target), &bus_index)) {
            return ESP_ERR_INVALID_ARG;
        }
        return append_claim(requests,
                            request_count,
                            SOLAR_OS_RESOURCE_I2C_ADDRESS,
                            (int)bus_index,
                            binding->value,
                            binding->target[0] != '\0' ? binding->target : "i2c");
    }
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT:
        /* The named UART bus owns its controller; the device owns a bus lease. */
        return ESP_OK;
    case SOLAR_OS_EXPANSION_BINDING_PS2_BUS:
        return ESP_OK;
    case SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM:
        return ESP_OK;
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
    case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
    default:
        return ESP_OK;
    }
}

static bool binding_valid(const solar_os_expansion_binding_t *binding,
                          const solar_os_expansion_binding_t *bindings,
                          size_t binding_count,
                          bool allow_board_pins)
{
    switch (binding->kind) {
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
        return pin_is_expansion_gpio(binding->value) ||
            (allow_board_pins &&
             solar_os_pin_get_info_by_pin(binding->value, NULL));
    case SOLAR_OS_EXPANSION_BINDING_GPIO_LINE: {
        if (binding->target[0] == '\0') {
            return pin_is_expansion_gpio(binding->value) ||
                (allow_board_pins &&
                 solar_os_pin_get_info_by_pin(binding->value, NULL));
        }
        solar_os_gpio_controller_info_t controller;
        return solar_os_gpio_controller_find(binding->target, &controller) &&
            binding->value >= 0 && binding->value < controller.line_count;
    }
    case SOLAR_OS_EXPANSION_BINDING_ADC:
        return pin_is_expansion_adc(binding->value) ||
            (allow_board_pins &&
             solar_os_pin_get_info_by_pin(binding->value, NULL));
    case SOLAR_OS_EXPANSION_BINDING_PWM:
        return pin_is_expansion_pwm(binding->value) ||
            (allow_board_pins &&
             solar_os_pin_get_info_by_pin(binding->value, NULL));
    case SOLAR_OS_EXPANSION_BINDING_I2S_PORT:
        return binding->value >= 0 && binding->value < SOC_I2S_NUM &&
            (allow_board_pins ||
             (solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_I2S) &&
              (SOLAR_OS_BOARD_RUNTIME_I2S_PORT_MASK &
               (1U << (uint32_t)binding->value)) != 0U));
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
        return solar_os_expansion_find_i2c_bus(binding->target, NULL, NULL);
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
        if (binding->value < 0x03 || binding->value > 0x77) {
            return false;
        }
        if (binding->target[0] != '\0') {
            return solar_os_expansion_find_i2c_bus(binding->target, NULL, NULL);
        }
        return first_i2c_binding(bindings, binding_count, NULL, 0, NULL);
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
        return solar_os_expansion_find_spi_bus(binding->target, NULL, NULL);
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
        return (pin_is_expansion_gpio(binding->value) ||
                (allow_board_pins &&
                 solar_os_pin_get_info_by_pin(binding->value, NULL))) &&
            solar_os_expansion_spi_cs_allowed(binding->target[0] != '\0' ? binding->target : NULL,
                                              binding->value);
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT: {
        solar_os_expansion_uart_port_t port;
        return solar_os_expansion_find_uart_port(binding->target, &port, NULL) &&
            port.port == binding->value;
    }
    case SOLAR_OS_EXPANSION_BINDING_PS2_BUS:
        return solar_os_bus_find(binding->target,
                                 SOLAR_OS_BUS_PROTOCOL_PS2,
                                 NULL);
    case SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM: {
        solar_os_stream_info_t info;
        return solar_os_stream_get_info(binding->target, &info) == ESP_OK &&
            info.type == SOLAR_OS_STREAM_TYPE_SCALAR &&
            info.direction != SOLAR_OS_STREAM_DIRECTION_SINK;
    }
    case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
        return binding->role[0] != '\0';
    default:
        return false;
    }
}

typedef struct {
    solar_os_bus_protocol_t protocol;
    const char *name;
} expansion_bus_ref_t;

static bool binding_bus_ref(const solar_os_expansion_binding_t *binding,
                            expansion_bus_ref_t *ref)
{
    if (binding == NULL || ref == NULL) {
        return false;
    }
    switch (binding->kind) {
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
        *ref = (expansion_bus_ref_t) {
            .protocol = SOLAR_OS_BUS_PROTOCOL_I2C,
            .name = binding->target,
        };
        return true;
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
        if (binding->target[0] == '\0') {
            return false;
        }
        *ref = (expansion_bus_ref_t) {
            .protocol = SOLAR_OS_BUS_PROTOCOL_I2C,
            .name = binding->target,
        };
        return true;
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
        *ref = (expansion_bus_ref_t) {
            .protocol = SOLAR_OS_BUS_PROTOCOL_SPI,
            .name = binding->target,
        };
        return true;
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT:
        *ref = (expansion_bus_ref_t) {
            .protocol = SOLAR_OS_BUS_PROTOCOL_UART,
            .name = binding->target,
        };
        return true;
    case SOLAR_OS_EXPANSION_BINDING_PS2_BUS:
        *ref = (expansion_bus_ref_t) {
            .protocol = SOLAR_OS_BUS_PROTOCOL_PS2,
            .name = binding->target,
        };
        return true;
    default:
        return false;
    }
}

static esp_err_t acquire_binding_buses(const solar_os_expansion_binding_t *bindings,
                                       size_t binding_count,
                                       const char *owner)
{
    expansion_bus_ref_t acquired[SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX];
    size_t acquired_count = 0;

    for (size_t i = 0; i < binding_count; i++) {
        expansion_bus_ref_t ref;
        if (!binding_bus_ref(&bindings[i], &ref)) {
            continue;
        }
        bool duplicate = false;
        for (size_t j = 0; j < acquired_count; j++) {
            if (acquired[j].protocol == ref.protocol &&
                strcmp(acquired[j].name, ref.name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        const esp_err_t ret = solar_os_bus_acquire(ref.name, ref.protocol, owner);
        if (ret != ESP_OK) {
            (void)solar_os_bus_release_owner(owner);
            return ret;
        }
        acquired[acquired_count++] = ref;
    }
    return ESP_OK;
}

static bool expansion_device_exists(const char *name)
{
    if (!devices_lock_take()) {
        return false;
    }
    const bool exists = find_device_locked(name) != NULL;
    devices_lock_give();
    return exists;
}

static esp_err_t expansion_init_board_defaults(bool early)
{
    ESP_RETURN_ON_ERROR(ensure_devices_mutex(), "expansion", "device registry init failed");
    ESP_RETURN_ON_ERROR(solar_os_resources_init(), "expansion", "resource init failed");
    ESP_RETURN_ON_ERROR(solar_os_buses_init(), "expansion", "bus init failed");
    esp_err_t first_error = ESP_OK;
#if SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT > 0
    for (size_t i = 0; i < SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT; i++) {
        const solar_os_expansion_default_device_t *device = &board_default_devices[i];
        const solar_os_expansion_driver_t *driver = find_driver(device->driver);
        if (driver == NULL || driver->early != early || expansion_device_exists(device->name)) {
            continue;
        }
        const esp_err_t err = expansion_attach(device->driver,
                                               device->name,
                                               device->bindings,
                                               device->binding_count,
                                               SOLAR_OS_EXPANSION_ORIGIN_BOARD,
                                               true,
                                               false);
        if (err != ESP_OK) {
            ESP_LOGW("expansion",
                     "Board device %s (%s) unavailable: %s",
                     device->name,
                     device->driver,
                     esp_err_to_name(err));
            if (first_error == ESP_OK) {
                first_error = err;
            }
        }
    }
#endif
    return first_error;
}

esp_err_t solar_os_expansion_init_early(void)
{
    return expansion_init_board_defaults(true);
}

esp_err_t solar_os_expansion_init(void)
{
    const esp_err_t ret = expansion_init_board_defaults(false);
    default_retry_pending = ret != ESP_OK;
    default_retry_scheduled = false;
    default_retry_attempts = 0U;
    default_retry_delay_ms = EXPANSION_DEFAULT_RETRY_INITIAL_MS;
    return ret;
}

void solar_os_expansion_poll(uint32_t now_ms)
{
    if (!default_retry_pending) {
        return;
    }
    if (!default_retry_scheduled) {
        default_retry_due_ms = now_ms + default_retry_delay_ms;
        default_retry_scheduled = true;
        return;
    }
    if ((int32_t)(now_ms - default_retry_due_ms) < 0) {
        return;
    }

    const esp_err_t ret = expansion_init_board_defaults(false);
    default_retry_attempts++;
    if (ret == ESP_OK) {
        default_retry_pending = false;
        default_retry_scheduled = false;
        ESP_LOGI("expansion", "Board expansion devices recovered");
        return;
    }
    if (default_retry_attempts >= EXPANSION_DEFAULT_RETRY_LIMIT) {
        default_retry_pending = false;
        default_retry_scheduled = false;
        ESP_LOGW("expansion", "Board expansion device retries exhausted");
        return;
    }

    if (default_retry_delay_ms < EXPANSION_DEFAULT_RETRY_MAX_MS) {
        default_retry_delay_ms *= 2U;
        if (default_retry_delay_ms > EXPANSION_DEFAULT_RETRY_MAX_MS) {
            default_retry_delay_ms = EXPANSION_DEFAULT_RETRY_MAX_MS;
        }
    }
    default_retry_due_ms = now_ms + default_retry_delay_ms;
}

bool solar_os_expansion_available(void)
{
    return solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_GPIO) ||
        solar_os_gpio_controller_count() > 0U ||
        solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_I2C) > 0 ||
        solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_SPI) > 0 ||
        solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_UART) > 0 ||
        solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_MIDI) > 0 ||
        solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_ONEWIRE) > 0 ||
        solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_PS2) > 0 ||
        solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_ADC) ||
        solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_PWM) ||
        solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_I2S);
}

size_t solar_os_expansion_driver_count(void)
{
    return sizeof(expansion_drivers) / sizeof(expansion_drivers[0]);
}

bool solar_os_expansion_get_driver(size_t index, solar_os_expansion_driver_t *driver)
{
    if (driver == NULL || index >= solar_os_expansion_driver_count()) {
        return false;
    }
    *driver = *expansion_drivers[index];
    return true;
}

const char *solar_os_expansion_category_name(solar_os_expansion_category_t category)
{
    static const char *const names[SOLAR_OS_EXPANSION_CATEGORY_COUNT] = {
        [SOLAR_OS_EXPANSION_CATEGORY_AUDIO] = "Audio",
        [SOLAR_OS_EXPANSION_CATEGORY_DISPLAY] = "Display",
        [SOLAR_OS_EXPANSION_CATEGORY_INPUT] = "Input",
        [SOLAR_OS_EXPANSION_CATEGORY_POWER] = "Power",
        [SOLAR_OS_EXPANSION_CATEGORY_RADIO] = "Radio",
        [SOLAR_OS_EXPANSION_CATEGORY_SENSOR] = "Sensor",
        [SOLAR_OS_EXPANSION_CATEGORY_STORAGE] = "Storage",
        [SOLAR_OS_EXPANSION_CATEGORY_UTILITY] = "Utility",
    };
    return (unsigned)category < SOLAR_OS_EXPANSION_CATEGORY_COUNT ?
        names[category] : "Utility";
}

bool solar_os_expansion_driver_supported(const char *name)
{
    const solar_os_expansion_driver_t *driver = find_driver(name);
    if (driver == NULL) {
        return false;
    }
    solar_os_board_capabilities_t caps = solar_os_board_capabilities();
    if (solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_I2C) > 0) {
        caps |= SOLAR_OS_BOARD_CAP_EXPANSION_I2C;
    }
    if (solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_SPI) > 0) {
        caps |= SOLAR_OS_BOARD_CAP_EXPANSION_SPI;
    }
    if (solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_UART) > 0 ||
        solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_MIDI) > 0) {
        caps |= SOLAR_OS_BOARD_CAP_EXPANSION_UART;
    }
    return driver->required_capabilities == 0 ||
        (caps & driver->required_capabilities) == driver->required_capabilities;
}

static esp_err_t validate_bindings(
    const char *driver,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count,
    solar_os_expansion_binding_validation_t *validation,
    bool allow_board_pins)
{
    if (validation != NULL) {
        memset(validation, 0, sizeof(*validation));
        validation->reason = SOLAR_OS_EXPANSION_BINDINGS_VALID;
    }
    if (driver == NULL || driver[0] == '\0' ||
        binding_count > SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX ||
        (binding_count > 0 && bindings == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    const solar_os_expansion_driver_t *driver_def = find_driver(driver);
    if (driver_def == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (driver_def->allow_unlisted_bindings && binding_count == 0) {
        set_binding_validation(validation, SOLAR_OS_EXPANSION_BINDINGS_MISSING, "resource");
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_spec_t *matched = NULL;
        size_t matches = 0;
        for (size_t s = 0; s < driver_def->binding_spec_count; s++) {
            const solar_os_expansion_binding_spec_t *spec = &driver_def->binding_specs[s];
            if (!binding_matches_spec(&bindings[i], spec)) {
                continue;
            }
            matched = spec;
            for (size_t j = 0; j < i; j++) {
                if (binding_matches_spec(&bindings[j], spec)) {
                    matches++;
                }
            }
            break;
        }
        if (matched == NULL &&
            (!driver_def->allow_unlisted_bindings ||
             bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER)) {
            set_binding_validation(validation,
                                   SOLAR_OS_EXPANSION_BINDINGS_UNEXPECTED,
                                   binding_key(&bindings[i]));
            return ESP_ERR_INVALID_ARG;
        }
        if (matched != NULL && matches > 0) {
            set_binding_validation(validation,
                                   SOLAR_OS_EXPANSION_BINDINGS_DUPLICATE,
                                   matched->key);
            return ESP_ERR_INVALID_ARG;
        }
        if (matched != NULL && matched->allowed_value_count > 0) {
            bool allowed = false;
            for (size_t v = 0; v < matched->allowed_value_count; v++) {
                if (bindings[i].value == matched->allowed_values[v]) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed) {
                set_binding_validation(validation,
                                       SOLAR_OS_EXPANSION_BINDINGS_INVALID_VALUE,
                                       matched->key);
                return ESP_ERR_INVALID_ARG;
            }
        }
        if (matched != NULL && matched->has_value_range &&
            (bindings[i].value < matched->min_value ||
             bindings[i].value > matched->max_value)) {
            set_binding_validation(validation,
                                   SOLAR_OS_EXPANSION_BINDINGS_INVALID_VALUE,
                                   matched->key);
            return ESP_ERR_INVALID_ARG;
        }
    }

    for (size_t s = 0; s < driver_def->binding_spec_count; s++) {
        const solar_os_expansion_binding_spec_t *spec = &driver_def->binding_specs[s];
        if (!spec->required) {
            continue;
        }
        bool found = false;
        for (size_t i = 0; i < binding_count; i++) {
            if (binding_matches_spec(&bindings[i], spec)) {
                found = true;
                break;
            }
        }
        if (!found) {
            set_binding_validation(validation,
                                   SOLAR_OS_EXPANSION_BINDINGS_MISSING,
                                   spec->key);
            return ESP_ERR_INVALID_ARG;
        }
    }

    for (size_t i = 0; i < binding_count; i++) {
        if (!binding_valid(&bindings[i],
                           bindings,
                           binding_count,
                           allow_board_pins)) {
            set_binding_validation(validation,
                                   SOLAR_OS_EXPANSION_BINDINGS_UNAVAILABLE,
                                   binding_key(&bindings[i]));
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_expansion_validate_bindings(
    const char *driver,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count,
    solar_os_expansion_binding_validation_t *validation)
{
    return validate_bindings(driver,
                             bindings,
                             binding_count,
                             validation,
                             false);
}

size_t solar_os_expansion_i2c_bus_count(void)
{
    return solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_I2C);
}

bool solar_os_expansion_get_i2c_bus(size_t index, solar_os_expansion_i2c_bus_t *bus)
{
    if (bus == NULL) {
        return false;
    }
    solar_os_bus_info_t info;
    if (!solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_I2C, index, &info)) {
        return false;
    }
    *bus = (solar_os_expansion_i2c_bus_t) {
        .port = info.config.i2c.port,
        .sda_pin = info.config.i2c.sda_pin,
        .scl_pin = info.config.i2c.scl_pin,
        .speed_hz = info.config.i2c.speed_hz,
    };
    strlcpy(bus->name, info.name, sizeof(bus->name));
    return true;
}

bool solar_os_expansion_find_i2c_bus(const char *name, solar_os_expansion_i2c_bus_t *bus, size_t *index)
{
    solar_os_bus_info_t info;
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_I2C, &info)) {
        return false;
    }
    if (bus != NULL) {
        *bus = (solar_os_expansion_i2c_bus_t) {
            .port = info.config.i2c.port,
            .sda_pin = info.config.i2c.sda_pin,
            .scl_pin = info.config.i2c.scl_pin,
            .speed_hz = info.config.i2c.speed_hz,
        };
        strlcpy(bus->name, info.name, sizeof(bus->name));
    }
    if (index != NULL) {
        *index = info.id;
    }
    return true;
}

size_t solar_os_expansion_spi_bus_count(void)
{
    return solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_SPI);
}

bool solar_os_expansion_get_spi_bus(size_t index, solar_os_expansion_spi_bus_t *bus)
{
    if (bus == NULL) {
        return false;
    }
    solar_os_bus_info_t info;
    if (!solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_SPI, index, &info)) {
        return false;
    }
    *bus = (solar_os_expansion_spi_bus_t) {
        .host = info.config.spi.host,
        .sclk_pin = info.config.spi.sclk_pin,
        .miso_pin = info.config.spi.miso_pin,
        .mosi_pin = info.config.spi.mosi_pin,
        .max_transfer_size = info.config.spi.max_transfer_size,
        .cs_count = info.config.spi.cs_count,
    };
    strlcpy(bus->name, info.name, sizeof(bus->name));
    for (size_t i = 0; i < info.config.spi.cs_count && i < SOLAR_OS_EXPANSION_SPI_CS_MAX; i++) {
        bus->cs[i] = info.config.spi.cs[i];
    }
    return true;
}

bool solar_os_expansion_find_spi_bus(const char *name, solar_os_expansion_spi_bus_t *bus, size_t *index)
{
    solar_os_bus_info_t info;
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_SPI, &info)) {
        return false;
    }
    if (bus != NULL) {
        *bus = (solar_os_expansion_spi_bus_t) {
            .host = info.config.spi.host,
            .sclk_pin = info.config.spi.sclk_pin,
            .miso_pin = info.config.spi.miso_pin,
            .mosi_pin = info.config.spi.mosi_pin,
            .max_transfer_size = info.config.spi.max_transfer_size,
            .cs_count = info.config.spi.cs_count,
        };
        strlcpy(bus->name, info.name, sizeof(bus->name));
        for (size_t i = 0; i < info.config.spi.cs_count && i < SOLAR_OS_EXPANSION_SPI_CS_MAX; i++) {
            bus->cs[i] = info.config.spi.cs[i];
        }
    }
    if (index != NULL) {
        *index = info.id;
    }
    return true;
}

bool solar_os_expansion_spi_cs_allowed(const char *bus_name, int pin)
{
    for (size_t i = 0; i < solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_SPI); i++) {
        solar_os_bus_info_t bus;
        if (!solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_SPI, i, &bus) ||
            (bus_name != NULL && strcmp(bus.name, bus_name) != 0)) {
            continue;
        }
        for (size_t cs = 0;
             cs < bus.config.spi.cs_count && cs < SOLAR_OS_EXPANSION_SPI_CS_MAX;
             cs++) {
            if (bus.config.spi.cs[cs].pin == pin) {
                return true;
            }
        }
    }
    return false;
}

size_t solar_os_expansion_uart_port_count(void)
{
    return solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_UART);
}

bool solar_os_expansion_get_uart_port(size_t index, solar_os_expansion_uart_port_t *port)
{
    if (port == NULL) {
        return false;
    }
    solar_os_bus_info_t info;
    if (!solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_UART, index, &info)) {
        return false;
    }
    *port = (solar_os_expansion_uart_port_t) {
        .port = info.config.uart.port,
        .tx_pin = info.config.uart.tx_pin,
        .rx_pin = info.config.uart.rx_pin,
        .baud_rate = info.config.uart.baud_rate,
    };
    strlcpy(port->name, info.name, sizeof(port->name));
    return true;
}

bool solar_os_expansion_find_uart_port(const char *name, solar_os_expansion_uart_port_t *port, size_t *index)
{
    solar_os_bus_info_t info;
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_UART, &info)) {
        return false;
    }
    if (port != NULL) {
        *port = (solar_os_expansion_uart_port_t) {
            .port = info.config.uart.port,
            .tx_pin = info.config.uart.tx_pin,
            .rx_pin = info.config.uart.rx_pin,
            .baud_rate = info.config.uart.baud_rate,
        };
        strlcpy(port->name, info.name, sizeof(port->name));
    }
    if (index != NULL) {
        *index = info.id;
    }
    return true;
}

size_t solar_os_expansion_onewire_bus_count(void)
{
    return solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_ONEWIRE);
}

bool solar_os_expansion_get_onewire_bus(size_t index, solar_os_expansion_onewire_bus_t *bus)
{
    if (bus == NULL) {
        return false;
    }
    solar_os_bus_info_t info;
    if (!solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_ONEWIRE, index, &info)) {
        return false;
    }
    *bus = (solar_os_expansion_onewire_bus_t) {
        .pin = info.config.onewire.pin,
    };
    strlcpy(bus->name, info.name, sizeof(bus->name));
    return true;
}

bool solar_os_expansion_find_onewire_bus(const char *name,
                                         solar_os_expansion_onewire_bus_t *bus,
                                         size_t *index)
{
    solar_os_bus_info_t info;
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_ONEWIRE, &info)) {
        return false;
    }
    if (bus != NULL) {
        *bus = (solar_os_expansion_onewire_bus_t) {
            .pin = info.config.onewire.pin,
        };
        strlcpy(bus->name, info.name, sizeof(bus->name));
    }
    if (index != NULL) {
        *index = info.id;
    }
    return true;
}

static esp_err_t expansion_attach(const char *driver,
                                  const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count,
                                  solar_os_expansion_origin_t origin,
                                  bool autostart,
                                  bool detachable)
{
    if (driver == NULL || driver[0] == '\0' ||
        !device_name_valid(name) ||
        binding_count > SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX ||
        (binding_count > 0 && bindings == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_expansion_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const solar_os_expansion_driver_t *driver_def = find_driver(driver);
    if (driver_def == NULL || !solar_os_expansion_driver_supported(driver)) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(validate_bindings(driver,
                                          bindings,
                                          binding_count,
                                          NULL,
                                          origin == SOLAR_OS_EXPANSION_ORIGIN_BOARD),
                        "expansion",
                        "invalid bindings");
    solar_os_expansion_binding_t normalized[SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX];
    for (size_t i = 0; i < binding_count; i++) {
        normalized[i] = bindings[i];
    }
    solar_os_resource_request_t requests[SOLAR_OS_RESOURCE_BUNDLE_MAX];
    size_t request_count = 0;
    for (size_t i = 0; i < binding_count; i++) {
        const esp_err_t ret = append_binding_claims(&normalized[i],
                                                    normalized,
                                                    binding_count,
                                                    requests,
                                                    &request_count);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    expansion_device_node_t *node = solar_os_memory_calloc(
        1,
        sizeof(*node),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "expansion.device");
    if (node == NULL) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(node->device.name, name, sizeof(node->device.name));
    strlcpy(node->device.driver, driver, sizeof(node->device.driver));
    node->device.origin = origin;
    node->device.autostart = autostart;
    node->device.detachable = detachable;
    node->device.binding_count = binding_count;
    memcpy(node->device.bindings,
           normalized,
           binding_count * sizeof(normalized[0]));
    node->state = EXPANSION_SLOT_ATTACHING;

    if (!devices_lock_take()) {
        solar_os_memory_free(node);
        return ESP_ERR_NO_MEM;
    }
    if (find_device_locked(name) != NULL) {
        devices_lock_give();
        solar_os_memory_free(node);
        return ESP_ERR_INVALID_STATE;
    }
    append_device_locked(node);
    devices_lock_give();

    if (request_count > 0) {
        const esp_err_t ret = solar_os_resource_claim_bundle(requests,
                                                             request_count,
                                                             name,
                                                             NULL);
        if (ret != ESP_OK) {
            release_device_reservation(node);
            return ret;
        }
    }

    const esp_err_t bus_ret = acquire_binding_buses(normalized, binding_count, name);
    if (bus_ret != ESP_OK) {
        (void)solar_os_resource_release_owner(name);
        release_device_reservation(node);
        return bus_ret;
    }

    if (driver_def->attach != NULL) {
        const esp_err_t ret = driver_def->attach(name, normalized, binding_count);
        if (ret != ESP_OK) {
            (void)solar_os_bus_release_owner(name);
            (void)solar_os_resource_release_owner(name);
            release_device_reservation(node);
            return ret;
        }
    }

    if (!devices_lock_take()) {
        (void)solar_os_bus_release_owner(name);
        (void)solar_os_resource_release_owner(name);
        release_device_reservation(node);
        return ESP_ERR_NO_MEM;
    }
    if (node->state != EXPANSION_SLOT_ATTACHING) {
        devices_lock_give();
        (void)solar_os_bus_release_owner(name);
        (void)solar_os_resource_release_owner(name);
        return ESP_ERR_INVALID_STATE;
    }
    node->device.active = true;
    node->device.ready = true;
    node->state = EXPANSION_SLOT_ACTIVE;
    devices_lock_give();
    return ESP_OK;
}

esp_err_t solar_os_expansion_attach(const char *driver,
                                    const char *name,
                                    const solar_os_expansion_binding_t *bindings,
                                    size_t binding_count)
{
    return expansion_attach(driver,
                            name,
                            bindings,
                            binding_count,
                            SOLAR_OS_EXPANSION_ORIGIN_RUNTIME,
                            false,
                            true);
}

esp_err_t solar_os_expansion_detach(const char *name)
{
    solar_os_expansion_device_t device;
    expansion_device_node_t *node = NULL;

    if (!devices_lock_take()) {
        return ESP_ERR_NO_MEM;
    }
    node = find_device_locked(name);
    if (node == NULL) {
        devices_lock_give();
        return ESP_ERR_NOT_FOUND;
    }
    if (node->state != EXPANSION_SLOT_ACTIVE) {
        devices_lock_give();
        return ESP_ERR_INVALID_STATE;
    }
    if (!node->device.detachable) {
        devices_lock_give();
        return ESP_ERR_NOT_SUPPORTED;
    }
    device = node->device;
    node->device.active = false;
    node->state = EXPANSION_SLOT_DETACHING;
    devices_lock_give();

    const solar_os_expansion_driver_t *driver = find_driver(device.driver);
    if (driver != NULL && driver->detach != NULL) {
        const esp_err_t ret = driver->detach(name);
        if (ret != ESP_OK) {
            if (devices_lock_take()) {
                if (node->state == EXPANSION_SLOT_DETACHING) {
                    node->device.active = true;
                    node->state = EXPANSION_SLOT_ACTIVE;
                }
                devices_lock_give();
            }
            return ret;
        }
    }

    (void)solar_os_bus_release_owner(name);
    (void)solar_os_resource_release_owner(name);
    bool removed = false;
    if (devices_lock_take()) {
        if (node->state == EXPANSION_SLOT_DETACHING) {
            removed = remove_device_locked(node);
        }
        devices_lock_give();
    }
    if (removed) {
        solar_os_memory_free(node);
    }
    return removed ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_expansion_device_set_ready(const char *name, bool ready)
{
    if (!device_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;
    if (!devices_lock_take()) {
        return ESP_ERR_NO_MEM;
    }
    expansion_device_node_t *node = find_device_locked(name);
    if (node == NULL) {
        result = ESP_ERR_NOT_FOUND;
    } else if (node->state != EXPANSION_SLOT_ACTIVE) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        node->device.ready = ready;
    }
    devices_lock_give();
    return result;
}

size_t solar_os_expansion_device_count(void)
{
    size_t count = 0;
    if (!devices_lock_take()) {
        return 0;
    }
    for (expansion_device_node_t *node = devices; node != NULL; node = node->next) {
        if (node->state == EXPANSION_SLOT_ACTIVE) {
            count++;
        }
    }
    devices_lock_give();
    return count;
}

bool solar_os_expansion_get_device(size_t index, solar_os_expansion_device_t *device)
{
    size_t current = 0;
    if (device == NULL) {
        return false;
    }
    if (!devices_lock_take()) {
        return false;
    }
    for (expansion_device_node_t *node = devices; node != NULL; node = node->next) {
        if (node->state != EXPANSION_SLOT_ACTIVE) {
            continue;
        }
        if (current++ == index) {
            *device = node->device;
            devices_lock_give();
            return true;
        }
    }
    devices_lock_give();
    return false;
}

const char *solar_os_expansion_binding_kind_name(solar_os_expansion_binding_kind_t kind)
{
    switch (kind) {
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
        return "gpio";
    case SOLAR_OS_EXPANSION_BINDING_GPIO_LINE:
        return "gpio_line";
    case SOLAR_OS_EXPANSION_BINDING_ADC:
        return "adc";
    case SOLAR_OS_EXPANSION_BINDING_PWM:
        return "pwm";
    case SOLAR_OS_EXPANSION_BINDING_I2S_PORT:
        return "i2s";
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
        return "i2c";
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
        return "i2c_addr";
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
        return "spi";
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
        return "spi_cs";
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT:
        return "uart";
    case SOLAR_OS_EXPANSION_BINDING_PS2_BUS:
        return "ps2";
    case SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM:
        return "scalar";
    case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
        return "parameter";
    default:
        return "unknown";
    }
}

const char *solar_os_expansion_origin_name(solar_os_expansion_origin_t origin)
{
    return origin == SOLAR_OS_EXPANSION_ORIGIN_RUNTIME ? "runtime" : "board";
}
