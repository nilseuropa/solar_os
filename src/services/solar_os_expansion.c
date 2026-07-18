#include "solar_os_expansion.h"

#include <string.h>

#include "esp_check.h"
#include "solar_os_board.h"
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_EXPANSION_PCD8544
#include "solar_os_pcd8544.h"
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_RFM69
#include "solar_os_rfm69.h"
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_SSD1306
#include "solar_os_ssd1306.h"
#endif
#include "solar_os_resources.h"

#define SOLAR_OS_EXPANSION_DEVICE_MAX 8

static const solar_os_expansion_i2c_bus_t i2c_buses[] = SOLAR_OS_BOARD_EXPANSION_I2C_BUSES;
static const solar_os_expansion_spi_bus_t spi_buses[] = SOLAR_OS_BOARD_EXPANSION_SPI_BUSES;
static const solar_os_expansion_uart_port_t uart_ports[] = SOLAR_OS_BOARD_EXPANSION_UART_PORTS;

static const solar_os_expansion_driver_t expansion_drivers[] = {
    {
        .name = "manual",
        .summary = "manual resource profile",
        .required_capabilities = 0,
        .probe_supported = false,
    },
#if SOLAR_OS_PACKAGE_EXPANSION_RFM69
    {
        .name = "rfm69",
        .summary = "HopeRF RFM69 SPI packet radio",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_SPI,
        .probe_supported = true,
        .attach = solar_os_rfm69_attach,
        .detach = solar_os_rfm69_detach,
    },
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_PCD8544
    {
        .name = "pcd8544",
        .summary = "PCD8544 84x48 SPI LCD",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_SPI |
            SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
        .probe_supported = false,
        .attach = solar_os_pcd8544_attach,
        .detach = solar_os_pcd8544_detach,
    },
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_SSD1306
    {
        .name = "ssd1306",
        .summary = "SSD1306 128x64 I2C OLED",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
        .probe_supported = true,
        .attach = solar_os_ssd1306_attach,
        .detach = solar_os_ssd1306_detach,
    },
    {
        .name = "sh1106",
        .summary = "SH1106 128x64 I2C OLED",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
        .probe_supported = true,
        .attach = solar_os_sh1106_attach,
        .detach = solar_os_ssd1306_detach,
    },
#endif
};

static solar_os_expansion_device_t devices[SOLAR_OS_EXPANSION_DEVICE_MAX];

static bool mask_contains(uint64_t mask, int pin)
{
    return pin >= 0 && pin < 64 && (mask & (1ULL << (uint32_t)pin)) != 0;
}

static bool i2c_bus_active(const solar_os_expansion_i2c_bus_t *bus)
{
    return bus != NULL && bus->name != NULL && bus->name[0] != '\0';
}

static bool spi_bus_active(const solar_os_expansion_spi_bus_t *bus)
{
    return bus != NULL && bus->name != NULL && bus->name[0] != '\0';
}

static bool uart_port_active(const solar_os_expansion_uart_port_t *port)
{
    return port != NULL && port->name != NULL && port->name[0] != '\0';
}

static bool device_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    return strnlen(name, SOLAR_OS_EXPANSION_DEVICE_NAME_MAX) < SOLAR_OS_EXPANSION_DEVICE_NAME_MAX;
}

static solar_os_expansion_device_t *find_device(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SOLAR_OS_EXPANSION_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

static solar_os_expansion_device_t *alloc_device(void)
{
    for (size_t i = 0; i < SOLAR_OS_EXPANSION_DEVICE_MAX; i++) {
        if (!devices[i].active) {
            return &devices[i];
        }
    }
    return NULL;
}

static const solar_os_expansion_driver_t *find_driver(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(expansion_drivers) / sizeof(expansion_drivers[0]); i++) {
        if (strcmp(expansion_drivers[i].name, name) == 0) {
            return &expansion_drivers[i];
        }
    }
    return NULL;
}

static bool pin_is_expansion_gpio(int pin)
{
    return solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_GPIO) &&
        mask_contains(SOLAR_OS_BOARD_USER_GPIO_MASK, pin);
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
        return append_claim(requests,
                            request_count,
                            SOLAR_OS_RESOURCE_UART_PORT,
                            binding->value,
                            -1,
                            binding->target);
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
    default:
        return ESP_OK;
    }
}

static bool binding_valid(const solar_os_expansion_binding_t *binding,
                          const solar_os_expansion_binding_t *bindings,
                          size_t binding_count)
{
    switch (binding->kind) {
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
        return pin_is_expansion_gpio(binding->value);
    case SOLAR_OS_EXPANSION_BINDING_ADC:
        return pin_is_expansion_adc(binding->value);
    case SOLAR_OS_EXPANSION_BINDING_PWM:
        return pin_is_expansion_pwm(binding->value);
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
        return solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_I2C) &&
            solar_os_expansion_find_i2c_bus(binding->target, NULL, NULL);
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
        if (binding->value < 0x03 || binding->value > 0x77) {
            return false;
        }
        if (binding->target[0] != '\0') {
            return solar_os_expansion_find_i2c_bus(binding->target, NULL, NULL);
        }
        return first_i2c_binding(bindings, binding_count, NULL, 0, NULL);
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
        return solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_SPI) &&
            solar_os_expansion_find_spi_bus(binding->target, NULL, NULL);
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
        return pin_is_expansion_gpio(binding->value) &&
            solar_os_expansion_spi_cs_allowed(binding->target[0] != '\0' ? binding->target : NULL,
                                              binding->value);
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT: {
        solar_os_expansion_uart_port_t port;
        return solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_UART) &&
            solar_os_expansion_find_uart_port(binding->target, &port, NULL) &&
            port.port == binding->value;
    }
    default:
        return false;
    }
}

esp_err_t solar_os_expansion_init(void)
{
    return solar_os_resources_init();
}

bool solar_os_expansion_available(void)
{
    return solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_GPIO) ||
        solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_I2C) ||
        solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_SPI) ||
        solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_UART) ||
        solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_ADC) ||
        solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_PWM);
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
    *driver = expansion_drivers[index];
    return true;
}

bool solar_os_expansion_driver_supported(const char *name)
{
    const solar_os_expansion_driver_t *driver = find_driver(name);
    if (driver == NULL) {
        return false;
    }
    const solar_os_board_capabilities_t caps = solar_os_board_capabilities();
    return driver->required_capabilities == 0 ||
        (caps & driver->required_capabilities) == driver->required_capabilities;
}

size_t solar_os_expansion_i2c_bus_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < sizeof(i2c_buses) / sizeof(i2c_buses[0]); i++) {
        if (i2c_bus_active(&i2c_buses[i])) {
            count++;
        }
    }
    return count;
}

bool solar_os_expansion_get_i2c_bus(size_t index, solar_os_expansion_i2c_bus_t *bus)
{
    size_t current = 0;
    if (bus == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(i2c_buses) / sizeof(i2c_buses[0]); i++) {
        if (!i2c_bus_active(&i2c_buses[i])) {
            continue;
        }
        if (current++ == index) {
            *bus = i2c_buses[i];
            return true;
        }
    }
    return false;
}

bool solar_os_expansion_find_i2c_bus(const char *name, solar_os_expansion_i2c_bus_t *bus, size_t *index)
{
    size_t current = 0;
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < sizeof(i2c_buses) / sizeof(i2c_buses[0]); i++) {
        if (!i2c_bus_active(&i2c_buses[i])) {
            continue;
        }
        if (strcmp(i2c_buses[i].name, name) == 0) {
            if (bus != NULL) {
                *bus = i2c_buses[i];
            }
            if (index != NULL) {
                *index = current;
            }
            return true;
        }
        current++;
    }
    return false;
}

size_t solar_os_expansion_spi_bus_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < sizeof(spi_buses) / sizeof(spi_buses[0]); i++) {
        if (spi_bus_active(&spi_buses[i])) {
            count++;
        }
    }
    return count;
}

bool solar_os_expansion_get_spi_bus(size_t index, solar_os_expansion_spi_bus_t *bus)
{
    size_t current = 0;
    if (bus == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(spi_buses) / sizeof(spi_buses[0]); i++) {
        if (!spi_bus_active(&spi_buses[i])) {
            continue;
        }
        if (current++ == index) {
            *bus = spi_buses[i];
            return true;
        }
    }
    return false;
}

bool solar_os_expansion_find_spi_bus(const char *name, solar_os_expansion_spi_bus_t *bus, size_t *index)
{
    size_t current = 0;
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < sizeof(spi_buses) / sizeof(spi_buses[0]); i++) {
        if (!spi_bus_active(&spi_buses[i])) {
            continue;
        }
        if (strcmp(spi_buses[i].name, name) == 0) {
            if (bus != NULL) {
                *bus = spi_buses[i];
            }
            if (index != NULL) {
                *index = current;
            }
            return true;
        }
        current++;
    }
    return false;
}

bool solar_os_expansion_spi_cs_allowed(const char *bus_name, int pin)
{
    for (size_t i = 0; i < sizeof(spi_buses) / sizeof(spi_buses[0]); i++) {
        const solar_os_expansion_spi_bus_t *bus = &spi_buses[i];
        if (!spi_bus_active(bus) || (bus_name != NULL && strcmp(bus->name, bus_name) != 0)) {
            continue;
        }
        for (size_t cs = 0; cs < bus->cs_count && cs < SOLAR_OS_EXPANSION_SPI_CS_MAX; cs++) {
            if (bus->cs[cs].pin == pin) {
                return true;
            }
        }
    }
    return false;
}

size_t solar_os_expansion_uart_port_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < sizeof(uart_ports) / sizeof(uart_ports[0]); i++) {
        if (uart_port_active(&uart_ports[i])) {
            count++;
        }
    }
    return count;
}

bool solar_os_expansion_get_uart_port(size_t index, solar_os_expansion_uart_port_t *port)
{
    size_t current = 0;
    if (port == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(uart_ports) / sizeof(uart_ports[0]); i++) {
        if (!uart_port_active(&uart_ports[i])) {
            continue;
        }
        if (current++ == index) {
            *port = uart_ports[i];
            return true;
        }
    }
    return false;
}

bool solar_os_expansion_find_uart_port(const char *name, solar_os_expansion_uart_port_t *port, size_t *index)
{
    size_t current = 0;
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < sizeof(uart_ports) / sizeof(uart_ports[0]); i++) {
        if (!uart_port_active(&uart_ports[i])) {
            continue;
        }
        if (strcmp(uart_ports[i].name, name) == 0) {
            if (port != NULL) {
                *port = uart_ports[i];
            }
            if (index != NULL) {
                *index = current;
            }
            return true;
        }
        current++;
    }
    return false;
}

esp_err_t solar_os_expansion_attach(const char *driver,
                                    const char *name,
                                    const solar_os_expansion_binding_t *bindings,
                                    size_t binding_count)
{
    if (driver == NULL || driver[0] == '\0' ||
        !device_name_valid(name) ||
        bindings == NULL ||
        binding_count == 0 ||
        binding_count > SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_expansion_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const solar_os_expansion_driver_t *driver_def = find_driver(driver);
    if (driver_def == NULL || !solar_os_expansion_driver_supported(driver)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (find_device(name) != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_expansion_binding_t normalized[SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX];
    for (size_t i = 0; i < binding_count; i++) {
        normalized[i] = bindings[i];
    }
    for (size_t i = 0; i < binding_count; i++) {
        if (!binding_valid(&normalized[i], normalized, binding_count)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    solar_os_expansion_device_t *device = alloc_device();
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
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
    if (request_count > 0) {
        const esp_err_t ret = solar_os_resource_claim_bundle(requests,
                                                             request_count,
                                                             name,
                                                             NULL);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (driver_def->attach != NULL) {
        const esp_err_t ret = driver_def->attach(name, normalized, binding_count);
        if (ret != ESP_OK) {
            (void)solar_os_resource_release_owner(name);
            return ret;
        }
    }

    memset(device, 0, sizeof(*device));
    device->active = true;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->driver, driver, sizeof(device->driver));
    device->binding_count = binding_count;
    for (size_t i = 0; i < binding_count; i++) {
        device->bindings[i] = normalized[i];
    }
    return ESP_OK;
}

esp_err_t solar_os_expansion_detach(const char *name)
{
    solar_os_expansion_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    const solar_os_expansion_driver_t *driver = find_driver(device->driver);
    if (driver != NULL && driver->detach != NULL) {
        const esp_err_t ret = driver->detach(name);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    (void)solar_os_resource_release_owner(name);
    memset(device, 0, sizeof(*device));
    return ESP_OK;
}

size_t solar_os_expansion_device_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < SOLAR_OS_EXPANSION_DEVICE_MAX; i++) {
        if (devices[i].active) {
            count++;
        }
    }
    return count;
}

bool solar_os_expansion_get_device(size_t index, solar_os_expansion_device_t *device)
{
    size_t current = 0;
    if (device == NULL) {
        return false;
    }
    for (size_t i = 0; i < SOLAR_OS_EXPANSION_DEVICE_MAX; i++) {
        if (!devices[i].active) {
            continue;
        }
        if (current++ == index) {
            *device = devices[i];
            return true;
        }
    }
    return false;
}

const char *solar_os_expansion_binding_kind_name(solar_os_expansion_binding_kind_t kind)
{
    switch (kind) {
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
        return "gpio";
    case SOLAR_OS_EXPANSION_BINDING_ADC:
        return "adc";
    case SOLAR_OS_EXPANSION_BINDING_PWM:
        return "pwm";
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
    default:
        return "unknown";
    }
}
