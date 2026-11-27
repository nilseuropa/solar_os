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

#define SOLAR_OS_DYNAMIC_EXPANSION_DRIVER_MAX 16U
static const solar_os_expansion_driver_t *dynamic_expansion_drivers[
    SOLAR_OS_DYNAMIC_EXPANSION_DRIVER_MAX];
static uint16_t driver_validations[SOLAR_OS_DYNAMIC_EXPANSION_DRIVER_MAX];
static portMUX_TYPE registry_init_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t drivers_mutex;
static StaticSemaphore_t drivers_mutex_storage;

typedef enum {
    EXPANSION_SLOT_ATTACHING,
    EXPANSION_SLOT_ACTIVE,
    EXPANSION_SLOT_DETACHING,
} expansion_slot_state_t;

typedef struct expansion_device_node {
    solar_os_expansion_device_t device;
    expansion_slot_state_t state;
    void *driver_instance;
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
/* An explicit detach must survive retries for other board peripherals. */
static bool board_device_detached[SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT];

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
                                  bool detachable,
                                  bool automatic);

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

static esp_err_t ensure_registry_mutex(SemaphoreHandle_t *mutex, StaticSemaphore_t *storage)
{
    portENTER_CRITICAL(&registry_init_lock);
    if (*mutex == NULL) *mutex = xSemaphoreCreateMutexStatic(storage);
    const bool available = *mutex != NULL;
    portEXIT_CRITICAL(&registry_init_lock);
    return available ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool drivers_lock_take(void)
{
    return ensure_registry_mutex(&drivers_mutex, &drivers_mutex_storage) == ESP_OK &&
        xSemaphoreTake(drivers_mutex, portMAX_DELAY) == pdTRUE;
}

static void drivers_lock_give(void)
{
    xSemaphoreGive(drivers_mutex);
}

static bool devices_lock_take(void)
{
    return ensure_registry_mutex(&devices_mutex, &devices_mutex_storage) == ESP_OK &&
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

static const solar_os_expansion_driver_t *find_builtin_driver(const char *name)
{
    for (size_t i = 0; i < sizeof(expansion_drivers) / sizeof(expansion_drivers[0]); i++) {
        if (strcmp(expansion_drivers[i]->name, name) == 0) return expansion_drivers[i];
    }
    return NULL;
}

static size_t find_dynamic_driver_locked(const char *name)
{
    for (size_t i = 0; i < SOLAR_OS_DYNAMIC_EXPANSION_DRIVER_MAX; i++) {
        if (dynamic_expansion_drivers[i] && strcmp(dynamic_expansion_drivers[i]->name, name) == 0) return i;
    }
    return SOLAR_OS_DYNAMIC_EXPANSION_DRIVER_MAX;
}

static const solar_os_expansion_driver_t *find_driver(const char *name)
{
    if (!name) return NULL;
    const solar_os_expansion_driver_t *driver = find_builtin_driver(name);
    if (driver || !drivers_lock_take()) return driver;
    const size_t slot = find_dynamic_driver_locked(name);
    if (slot < SOLAR_OS_DYNAMIC_EXPANSION_DRIVER_MAX) driver = dynamic_expansion_drivers[slot];
    drivers_lock_give();
    return driver;
}

static bool copy_driver_capabilities(const char *name, solar_os_board_capabilities_t *required)
{
    if (!name) return false;
    const solar_os_expansion_driver_t *builtin = find_builtin_driver(name);
    if (builtin) {
        *required = builtin->required_capabilities;
        return true;
    }
    if (!drivers_lock_take()) return false;
    const size_t slot = find_dynamic_driver_locked(name);
    const bool found = slot < SOLAR_OS_DYNAMIC_EXPANSION_DRIVER_MAX;
    if (found) *required = dynamic_expansion_drivers[slot]->required_capabilities;
    drivers_lock_give();
    return found;
}

bool solar_os_expansion_stream_binding_valid(solar_os_expansion_binding_kind_t kind,
                                             const solar_os_stream_info_t *info)
{
    if (!info) return false;
    if (kind == SOLAR_OS_EXPANSION_BINDING_BYTE_STREAM)
        return info->type == SOLAR_OS_STREAM_TYPE_BYTES &&
            info->direction == SOLAR_OS_STREAM_DIRECTION_DUPLEX &&
            info->sharing == SOLAR_OS_STREAM_SHARING_EXCLUSIVE;
    return kind == SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM &&
        info->type == SOLAR_OS_STREAM_TYPE_SCALAR && info->direction != SOLAR_OS_STREAM_DIRECTION_SINK;
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
    case SOLAR_OS_EXPANSION_BINDING_BYTE_STREAM:
    case SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM:
    case SOLAR_OS_EXPANSION_BINDING_DISPLAY_TARGET:
        return binding->role[0] != '\0' ? binding->role : "stream";
    case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
        return binding->role[0] != '\0' ? binding->role : "parameter";
    default:
        return "resource";
    }
}

static esp_err_t driver_attach_callback(
    const solar_os_expansion_driver_t *driver,
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count,
    void **instance)
{
    if (driver->attach_with_user != NULL) {
        return driver->attach_with_user(driver->callback_user,
                                        name,
                                        bindings,
                                        binding_count,
                                        instance);
    }
    return driver->attach != NULL ?
        driver->attach(name, bindings, binding_count) : ESP_OK;
}

static esp_err_t driver_detach_callback(
    const solar_os_expansion_driver_t *driver,
    const char *name,
    void *instance)
{
    if (driver->detach_with_user != NULL) {
        return driver->detach_with_user(driver->callback_user, name, instance);
    }
    return driver->detach != NULL ? driver->detach(name) : ESP_OK;
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
    case SOLAR_OS_EXPANSION_BINDING_DISPLAY_TARGET:
        return ESP_OK;
    case SOLAR_OS_EXPANSION_BINDING_BYTE_STREAM:
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
    case SOLAR_OS_EXPANSION_BINDING_BYTE_STREAM:
    case SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM: {
        solar_os_stream_info_t info;
        return solar_os_stream_get_info(binding->target, &info) == ESP_OK &&
            solar_os_expansion_stream_binding_valid(binding->kind, &info);
    }
    case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
        return binding->role[0] != '\0';
    case SOLAR_OS_EXPANSION_BINDING_DISPLAY_TARGET:
        /* The consumer checks target existence and geometry during attachment;
         * this is a reference, not an exclusive display ownership claim. */
        return binding->target[0] != '\0' && binding->role[0] != '\0';
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
    ESP_RETURN_ON_ERROR(ensure_registry_mutex(&devices_mutex, &devices_mutex_storage), "expansion", "device registry init failed");
    ESP_RETURN_ON_ERROR(solar_os_resources_init(), "expansion", "resource init failed");
    ESP_RETURN_ON_ERROR(solar_os_buses_init(), "expansion", "bus init failed");
    esp_err_t first_error = ESP_OK;
#if SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT > 0
    for (size_t i = 0; i < SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT; i++) {
        const solar_os_expansion_default_device_t *device = &board_default_devices[i];
        if (!devices_lock_take()) {
            return ESP_ERR_NO_MEM;
        }
        const bool detached = board_device_detached[i];
        devices_lock_give();
        const solar_os_expansion_driver_t *driver = find_driver(device->driver);
        if (detached || driver == NULL || driver->early != early || expansion_device_exists(device->name)) {
            continue;
        }
        const esp_err_t err = expansion_attach(device->driver,
                                               device->name,
                                               device->bindings,
                                               device->binding_count,
                                               SOLAR_OS_EXPANSION_ORIGIN_BOARD,
                                               true,
                                               device->detachable,
                                               true);
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
    size_t count = 0;
    for (size_t i = 0; i < sizeof(expansion_drivers) / sizeof(expansion_drivers[0]); i++) {
        if (!expansion_drivers[i]->board_only) {
            count++;
        }
    }
    if (!drivers_lock_take()) {
        return count;
    }
    for (size_t i = 0; i < SOLAR_OS_DYNAMIC_EXPANSION_DRIVER_MAX; i++) {
        if (dynamic_expansion_drivers[i] != NULL) {
            count++;
        }
    }
    drivers_lock_give();
    return count;
}

static const solar_os_expansion_driver_t *builtin_driver_at(size_t *index)
{
    for (size_t i = 0; i < sizeof(expansion_drivers) / sizeof(expansion_drivers[0]); ++i) {
        if (expansion_drivers[i]->board_only) continue;
        if (*index == 0) return expansion_drivers[i];
        --*index;
    }
    return NULL;
}

static const solar_os_expansion_driver_t *dynamic_driver_at_locked(size_t index)
{
    for (size_t i = 0; i < SOLAR_OS_DYNAMIC_EXPANSION_DRIVER_MAX; ++i) {
        if (!dynamic_expansion_drivers[i]) continue;
        if (index-- == 0) return dynamic_expansion_drivers[i];
    }
    return NULL;
}

typedef esp_err_t (*read_driver_fn)(const solar_os_expansion_driver_t *, void *);

/* The reader runs while the dynamic descriptor is stable. It only copies
 * metadata and does not invoke driver or application callbacks. */
static esp_err_t read_driver(const char *name, size_t index, read_driver_fn read, void *out)
{
    const solar_os_expansion_driver_t *driver = name ? find_builtin_driver(name) : builtin_driver_at(&index);
    if (driver) return driver->board_only ? ESP_ERR_NOT_FOUND : read(driver, out);
    if (!drivers_lock_take()) return ESP_ERR_NO_MEM;
    if (name) {
        const size_t slot = find_dynamic_driver_locked(name);
        if (slot < SOLAR_OS_DYNAMIC_EXPANSION_DRIVER_MAX) driver = dynamic_expansion_drivers[slot];
    } else {
        driver = dynamic_driver_at_locked(index);
    }
    const esp_err_t result = driver ? read(driver, out) : ESP_ERR_NOT_FOUND;
    drivers_lock_give();
    return result;
}

static esp_err_t copy_driver(const solar_os_expansion_driver_t *source, void *out)
{
    *(solar_os_expansion_driver_t *)out = *source;
    return ESP_OK;
}

bool solar_os_expansion_get_driver(size_t index, solar_os_expansion_driver_t *driver)
{
    return driver && read_driver(NULL, index, copy_driver, driver) == ESP_OK;
}

static bool copy_text(const char *text, char *out, size_t capacity)
{
    if (!text) return true;
    const size_t length = strnlen(text, capacity);
    if (length == capacity) return false;
    memcpy(out, text, length + 1U);
    return true;
}

static esp_err_t copy_driver_info(const solar_os_expansion_driver_t *driver, void *out)
{
    solar_os_expansion_driver_info_t info = {
        .required_capabilities = driver->required_capabilities,
        .category = driver->category, .binding_spec_count = driver->binding_spec_count,
        .probe_supported = driver->probe_supported, .allow_unlisted_bindings = driver->allow_unlisted_bindings,
    };
    if (!copy_text(driver->name, info.name, sizeof(info.name)) ||
        !copy_text(driver->summary, info.summary, sizeof(info.summary))) return ESP_ERR_INVALID_SIZE;
    *(solar_os_expansion_driver_info_t *)out = info;
    return ESP_OK;
}

esp_err_t solar_os_expansion_get_driver_info(size_t index, solar_os_expansion_driver_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    return read_driver(NULL, index, copy_driver_info, info);
}

esp_err_t solar_os_expansion_find_driver_info(const char *name, solar_os_expansion_driver_info_t *info)
{
    if (!name || !*name || !info) return ESP_ERR_INVALID_ARG;
    return read_driver(name, 0, copy_driver_info, info);
}

typedef struct {
    size_t spec_index, capacity;
    solar_os_expansion_binding_spec_info_t *spec;
    int *values;
} binding_spec_request_t;

static esp_err_t copy_binding_spec(const solar_os_expansion_driver_t *driver, void *user)
{
    const binding_spec_request_t *request = user;
    if (request->spec_index >= driver->binding_spec_count) return ESP_ERR_NOT_FOUND;
    const solar_os_expansion_binding_spec_t *binding = &driver->binding_specs[request->spec_index];
    if (binding->allowed_value_count > SIZE_MAX / sizeof(int) ||
        (binding->allowed_value_count != 0 && !binding->allowed_values)) return ESP_ERR_INVALID_STATE;
    solar_os_expansion_binding_spec_info_t info = {
        .kind = binding->kind, .required = binding->required,
        .has_role = binding->role != NULL, .has_value_hint = binding->value_hint != NULL,
        .allowed_value_count = binding->allowed_value_count, .has_value_range = binding->has_value_range,
        .min_value = binding->min_value, .max_value = binding->max_value,
    };
    if (!copy_text(binding->key, info.key, sizeof(info.key)) ||
        !copy_text(binding->role, info.role, sizeof(info.role)) ||
        !copy_text(binding->value_hint, info.value_hint, sizeof(info.value_hint))) return ESP_ERR_INVALID_SIZE;
    *request->spec = info;
    if (request->values && info.allowed_value_count) {
        if (request->capacity < info.allowed_value_count) return ESP_ERR_INVALID_SIZE;
        memcpy(request->values, binding->allowed_values, info.allowed_value_count * sizeof(*request->values));
    }
    return ESP_OK;
}

esp_err_t solar_os_expansion_get_binding_spec(const char *name, size_t index,
    solar_os_expansion_binding_spec_info_t *out, int *values, size_t capacity)
{
    if (!name || !*name || !out || (!values && capacity)) return ESP_ERR_INVALID_ARG;
    binding_spec_request_t request = { .spec_index = index, .capacity = capacity, .spec = out, .values = values };
    return read_driver(name, 0, copy_binding_spec, &request);
}

esp_err_t solar_os_expansion_register_driver(
    const solar_os_expansion_driver_t *driver)
{
    if (driver == NULL || driver->name == NULL || driver->name[0] == '\0' ||
        strnlen(driver->name, SOLAR_OS_EXPANSION_DRIVER_NAME_MAX) >= SOLAR_OS_EXPANSION_DRIVER_NAME_MAX ||
        driver->summary == NULL || driver->early || driver->board_only ||
        driver->binding_spec_count > SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX ||
        (driver->binding_spec_count != 0U && !driver->binding_specs) || driver->allow_unlisted_bindings) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < driver->binding_spec_count; i++) {
        const solar_os_expansion_binding_spec_t *spec = &driver->binding_specs[i];
        if (!spec->key || !*spec->key || strnlen(spec->key, SOLAR_OS_EXPANSION_ROLE_MAX) >= SOLAR_OS_EXPANSION_ROLE_MAX ||
            (spec->role && strnlen(spec->role, SOLAR_OS_EXPANSION_ROLE_MAX) >= SOLAR_OS_EXPANSION_ROLE_MAX) ||
            (spec->value_hint && strnlen(spec->value_hint, SOLAR_OS_EXPANSION_BINDING_HINT_MAX) >= SOLAR_OS_EXPANSION_BINDING_HINT_MAX) ||
            (unsigned)spec->kind > SOLAR_OS_EXPANSION_BINDING_DISPLAY_TARGET ||
            (spec->allowed_value_count && !spec->allowed_values) ||
            (spec->has_value_range && spec->min_value > spec->max_value)) return ESP_ERR_INVALID_ARG;
    }
    if (find_builtin_driver(driver->name)) return ESP_ERR_INVALID_STATE;
    if (!drivers_lock_take()) {
        return ESP_ERR_NO_MEM;
    }
    size_t free_slot = SOLAR_OS_DYNAMIC_EXPANSION_DRIVER_MAX;
    esp_err_t result = ESP_ERR_NO_MEM;
    for (size_t i = 0; i < SOLAR_OS_DYNAMIC_EXPANSION_DRIVER_MAX; i++) {
        const solar_os_expansion_driver_t *registered = dynamic_expansion_drivers[i];
        if (!registered) {
            if (free_slot == SOLAR_OS_DYNAMIC_EXPANSION_DRIVER_MAX) free_slot = i;
        } else if (strcmp(registered->name, driver->name) == 0) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
    }
    if (result == ESP_ERR_NO_MEM && free_slot < SOLAR_OS_DYNAMIC_EXPANSION_DRIVER_MAX) {
        dynamic_expansion_drivers[free_slot] = driver;
        result = ESP_OK;
    }
    drivers_lock_give();
    return result;
}

esp_err_t solar_os_expansion_unregister_driver(
    const solar_os_expansion_driver_t *driver)
{
    if (driver == NULL || driver->name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Board-device refresh already looks up a driver under the device lock.
     * Remove drivers in that same lock order, with no unpublished slot gap. */
    if (!devices_lock_take()) {
        return ESP_ERR_NO_MEM;
    }
    if (!drivers_lock_take()) {
        devices_lock_give();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < SOLAR_OS_DYNAMIC_EXPANSION_DRIVER_MAX; i++) {
        if (dynamic_expansion_drivers[i] != driver) continue;
        if (driver_validations[i]) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        result = ESP_OK;
        for (const expansion_device_node_t *node = devices; node != NULL; node = node->next) {
            if (strcmp(node->device.driver, driver->name) == 0) {
                result = ESP_ERR_INVALID_STATE;
                break;
            }
        }
        if (result == ESP_OK) dynamic_expansion_drivers[i] = NULL;
        break;
    }
    drivers_lock_give();
    devices_lock_give();
    return result;
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
    solar_os_board_capabilities_t required;
    if (!copy_driver_capabilities(name, &required)) {
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
    return (caps & required) == required;
}

bool solar_os_expansion_binding_pin_supported(
    solar_os_expansion_binding_kind_t kind,
    const char *target,
    int pin)
{
    switch (kind) {
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
    case SOLAR_OS_EXPANSION_BINDING_GPIO_LINE:
        return pin_is_expansion_gpio(pin);
    case SOLAR_OS_EXPANSION_BINDING_ADC:
        return pin_is_expansion_adc(pin);
    case SOLAR_OS_EXPANSION_BINDING_PWM:
        return pin_is_expansion_pwm(pin);
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
        return pin_is_expansion_gpio(pin) &&
            solar_os_expansion_spi_cs_allowed(
                target != NULL && target[0] != '\0' ? target : NULL,
                pin);
    default:
        return false;
    }
}

static esp_err_t validate_driver_bindings(
    const solar_os_expansion_driver_t *driver_def,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count,
    solar_os_expansion_binding_validation_t *validation,
    bool allow_board_pins)
{
    if (driver_def->board_only && !allow_board_pins) {
        return ESP_ERR_NOT_SUPPORTED;
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

    if (driver_def->validate_bindings != NULL) {
        const esp_err_t ret = driver_def->validate_bindings(bindings,
                                                            binding_count,
                                                            validation);
        if (ret != ESP_OK) {
            return ret;
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

static esp_err_t validate_bindings(const char *driver, const solar_os_expansion_binding_t *bindings,
    size_t binding_count, solar_os_expansion_binding_validation_t *validation, bool allow_board_pins)
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

    for (size_t i = 0; i < binding_count; ++i) {
        if (!memchr(bindings[i].role, 0, sizeof(bindings[i].role)) ||
            !memchr(bindings[i].target, 0, sizeof(bindings[i].target))) {
            set_binding_validation(validation, SOLAR_OS_EXPANSION_BINDINGS_INVALID_VALUE, "resource");
            return ESP_ERR_INVALID_ARG;
        }
    }
    const solar_os_expansion_driver_t *definition = find_builtin_driver(driver);
    if (definition) return validate_driver_bindings(definition, bindings, binding_count, validation, allow_board_pins);

    if (!drivers_lock_take()) return ESP_ERR_NO_MEM;
    const size_t slot = find_dynamic_driver_locked(driver);
    esp_err_t result = ESP_ERR_NOT_FOUND;
    if (slot < SOLAR_OS_DYNAMIC_EXPANSION_DRIVER_MAX) {
        result = ESP_ERR_INVALID_STATE;
        if (driver_validations[slot] < UINT16_MAX) {
            ++driver_validations[slot];
            definition = dynamic_expansion_drivers[slot];
        }
    }
    drivers_lock_give();
    if (!definition) return result;
    result = validate_driver_bindings(definition, bindings, binding_count, validation, allow_board_pins);
    if (drivers_lock_take()) {
        --driver_validations[slot];
        drivers_lock_give();
    }
    return result;
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
                                  bool detachable,
                                  bool automatic)
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
    solar_os_expansion_binding_t *normalized = node->device.bindings;
    if (binding_count > 0U) {
        memcpy(normalized, bindings, binding_count * sizeof(normalized[0]));
    }
    node->state = EXPANSION_SLOT_ATTACHING;

    if (!devices_lock_take()) {
        solar_os_memory_free(node);
        return ESP_ERR_NO_MEM;
    }
#if SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT > 0
    /* Recheck under the reservation lock: a user may have detached after
     * the startup retry inspected the defaults. */
    if (automatic) {
        for (size_t i = 0; i < SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT; i++) {
            if (board_device_detached[i] && strcmp(board_default_devices[i].name, name) == 0) {
                devices_lock_give();
                solar_os_memory_free(node);
                return ESP_OK;
            }
        }
    }
#else
    (void)automatic;
#endif
    if (find_device_locked(name) != NULL) {
        devices_lock_give();
        solar_os_memory_free(node);
        return ESP_ERR_INVALID_STATE;
    }
    append_device_locked(node);
    devices_lock_give();

    const solar_os_expansion_driver_t *driver_def = find_driver(driver);
    if (driver_def == NULL || !solar_os_expansion_driver_supported(driver)) {
        release_device_reservation(node);
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t validation_ret = validate_bindings(
        driver,
        bindings,
        binding_count,
        NULL,
        origin == SOLAR_OS_EXPANSION_ORIGIN_BOARD);
    if (validation_ret != ESP_OK) {
        release_device_reservation(node);
        ESP_LOGE("expansion", "invalid bindings: %s", esp_err_to_name(validation_ret));
        return validation_ret;
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
            release_device_reservation(node);
            return ret;
        }
    }

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

    if (driver_def->attach != NULL || driver_def->attach_with_user != NULL) {
        const esp_err_t ret = driver_attach_callback(driver_def,
                                                     name,
                                                     normalized,
                                                     binding_count,
                                                     &node->driver_instance);
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
#if SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT > 0
    if (origin == SOLAR_OS_EXPANSION_ORIGIN_BOARD) {
        for (size_t i = 0; i < SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT; i++) {
            if (strcmp(board_default_devices[i].name, name) == 0) {
                board_device_detached[i] = false;
                break;
            }
        }
    }
#endif
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
                            true,
                            false);
}

esp_err_t solar_os_expansion_attach_board_device(const char *name)
{
    if (!device_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
#if SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT > 0
    for (size_t i = 0; i < SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT; i++) {
        const solar_os_expansion_default_device_t *device = &board_default_devices[i];
        if (strcmp(device->name, name) != 0) {
            continue;
        }
        if (!devices_lock_take()) return ESP_ERR_NO_MEM;
        expansion_device_node_t *node = find_device_locked(name);
        if (node != NULL) {
            const solar_os_expansion_driver_t *driver = find_driver(device->driver);
            if (node->state != EXPANSION_SLOT_ACTIVE ||
                node->device.origin != SOLAR_OS_EXPANSION_ORIGIN_BOARD ||
                strcmp(node->device.driver, device->driver) != 0 || driver == NULL ||
                driver->category != SOLAR_OS_EXPANSION_CATEGORY_STORAGE) {
                devices_lock_give();
                return ESP_ERR_INVALID_STATE;
            }
            /* Keep the slot reserved while refreshing removable media. */
            node->state = EXPANSION_SLOT_ATTACHING;
            devices_lock_give();
            const esp_err_t ret = driver_attach_callback(driver, name,
                node->device.bindings, node->device.binding_count, &node->driver_instance);
            if (!devices_lock_take()) return ESP_ERR_NO_MEM;
            node->state = EXPANSION_SLOT_ACTIVE;
            node->device.ready = ret == ESP_OK;
            devices_lock_give();
            return ret;
        }
        devices_lock_give();
        const esp_err_t ret = expansion_attach(device->driver,
                                               device->name,
                                               device->bindings,
                                               device->binding_count,
                                               SOLAR_OS_EXPANSION_ORIGIN_BOARD,
                                               true,
                                               device->detachable,
                                               false);
        return ret;
    }
#endif
    return ESP_ERR_NOT_FOUND;
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
    if (driver != NULL &&
        (driver->detach != NULL || driver->detach_with_user != NULL)) {
        const esp_err_t ret = driver_detach_callback(driver, name, node->driver_instance);
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
#if SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT > 0
            if (removed && device.origin == SOLAR_OS_EXPANSION_ORIGIN_BOARD) {
                for (size_t i = 0; i < SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT; i++) {
                    if (strcmp(board_default_devices[i].name, name) == 0) {
                        board_device_detached[i] = true;
                        break;
                    }
                }
            }
#endif
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
    case SOLAR_OS_EXPANSION_BINDING_BYTE_STREAM:
        return "bytes";
    case SOLAR_OS_EXPANSION_BINDING_DISPLAY_TARGET:
        return "display";
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
