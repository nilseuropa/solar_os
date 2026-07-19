#include "solar_os_onewire.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "onewire_bus.h"
#include "onewire_device.h"
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
#include "solar_os_buses.h"
#endif
#include "solar_os_gpio.h"

#define ONEWIRE_SERVICE_OWNER "service-onewire"

static SemaphoreHandle_t onewire_mutex;
static StaticSemaphore_t onewire_mutex_buffer;

static esp_err_t onewire_finish_operation(const char *name, esp_err_t operation_err)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    const esp_err_t release_err = solar_os_bus_release(name,
                                                       SOLAR_OS_BUS_PROTOCOL_ONEWIRE,
                                                       ONEWIRE_SERVICE_OWNER);
    return operation_err == ESP_OK ? release_err : operation_err;
#else
    (void)name;
    return operation_err;
#endif
}

static esp_err_t onewire_ensure_init(void)
{
    if (onewire_mutex == NULL) {
        onewire_mutex = xSemaphoreCreateMutexStatic(&onewire_mutex_buffer);
    }
    return onewire_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t onewire_validate_pin(int pin)
{
    return solar_os_gpio_is_runtime_allowed(pin) ? ESP_OK : ESP_ERR_NOT_ALLOWED;
}

static esp_err_t onewire_open(int pin, onewire_bus_handle_t *bus)
{
    const onewire_bus_config_t bus_config = {
        .bus_gpio_num = pin,
        .flags.en_pull_up = true,
    };
    const onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = SOLAR_OS_ONEWIRE_MAX_TRANSFER,
    };
    return onewire_new_bus_rmt(&bus_config, &rmt_config, bus);
}

static esp_err_t onewire_close(int pin,
                               bool restore_runtime_pin,
                               onewire_bus_handle_t bus,
                               esp_err_t operation_err)
{
    const esp_err_t close_err = onewire_bus_del(bus);
    const esp_err_t gpio_err = restore_runtime_pin
        ? solar_os_gpio_configure(pin,
                                  SOLAR_OS_GPIO_MODE_INPUT,
                                  SOLAR_OS_GPIO_PULL_UP)
        : ESP_OK;
    if (operation_err != ESP_OK) {
        return operation_err;
    }
    if (close_err != ESP_OK) {
        return close_err;
    }
    return gpio_err;
}

esp_err_t solar_os_onewire_init(void)
{
    return onewire_ensure_init();
}

bool solar_os_onewire_pin_allowed(int pin)
{
    return solar_os_gpio_is_runtime_allowed(pin);
}

static esp_err_t onewire_reset(int pin, bool restore_runtime_pin, bool *present)
{
    if (present == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *present = false;

    esp_err_t err = onewire_ensure_init();
    if (err != ESP_OK) {
        return err;
    }
    if (restore_runtime_pin) {
        err = onewire_validate_pin(pin);
        if (err != ESP_OK) {
            return err;
        }
    }

    xSemaphoreTake(onewire_mutex, portMAX_DELAY);
    onewire_bus_handle_t bus = NULL;
    err = onewire_open(pin, &bus);
    if (err == ESP_OK) {
        err = onewire_bus_reset(bus);
        if (err == ESP_OK) {
            *present = true;
        } else if (err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;
        }
        err = onewire_close(pin, restore_runtime_pin, bus, err);
    }
    xSemaphoreGive(onewire_mutex);
    return err;
}

esp_err_t solar_os_onewire_reset(int pin, bool *present)
{
    return onewire_reset(pin, true, present);
}

esp_err_t solar_os_onewire_reset_configured(int pin, bool *present)
{
    return onewire_reset(pin, false, present);
}

static esp_err_t onewire_scan(int pin,
                              bool restore_runtime_pin,
                              uint64_t *addresses,
                              size_t max_addresses,
                              size_t *address_count)
{
    if (address_count == NULL || (addresses == NULL && max_addresses > 0) ||
        max_addresses > SOLAR_OS_ONEWIRE_MAX_DEVICES) {
        return ESP_ERR_INVALID_ARG;
    }
    *address_count = 0;

    esp_err_t err = onewire_ensure_init();
    if (err != ESP_OK) {
        return err;
    }
    if (restore_runtime_pin) {
        err = onewire_validate_pin(pin);
        if (err != ESP_OK) {
            return err;
        }
    }

    xSemaphoreTake(onewire_mutex, portMAX_DELAY);
    onewire_bus_handle_t bus = NULL;
    onewire_device_iter_handle_t iter = NULL;
    err = onewire_open(pin, &bus);
    if (err == ESP_OK) {
        err = onewire_new_device_iter(bus, &iter);
    }
    while (err == ESP_OK && *address_count < max_addresses) {
        onewire_device_t device;
        err = onewire_device_iter_get_next(iter, &device);
        if (err == ESP_OK) {
            addresses[(*address_count)++] = device.address;
        }
    }
    if (err == ESP_ERR_NOT_FOUND) {
        err = ESP_OK;
    }
    if (iter != NULL) {
        const esp_err_t iter_err = onewire_del_device_iter(iter);
        if (err == ESP_OK) {
            err = iter_err;
        }
    }
    if (bus != NULL) {
        err = onewire_close(pin, restore_runtime_pin, bus, err);
    }
    xSemaphoreGive(onewire_mutex);
    return err;
}

esp_err_t solar_os_onewire_scan(int pin,
                                uint64_t *addresses,
                                size_t max_addresses,
                                size_t *address_count)
{
    return onewire_scan(pin, true, addresses, max_addresses, address_count);
}

esp_err_t solar_os_onewire_scan_configured(int pin,
                                           uint64_t *addresses,
                                           size_t max_addresses,
                                           size_t *address_count)
{
    return onewire_scan(pin, false, addresses, max_addresses, address_count);
}

static esp_err_t onewire_transfer(int pin,
                                  bool restore_runtime_pin,
                                  const uint8_t *tx_data,
                                  size_t tx_len,
                                  uint8_t *rx_data,
                                  size_t rx_len)
{
    if ((tx_data == NULL && tx_len > 0) || (rx_data == NULL && rx_len > 0) ||
        (tx_len == 0 && rx_len == 0) || tx_len > SOLAR_OS_ONEWIRE_MAX_TRANSFER ||
        rx_len > SOLAR_OS_ONEWIRE_MAX_TRANSFER) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = onewire_ensure_init();
    if (err != ESP_OK) {
        return err;
    }
    if (restore_runtime_pin) {
        err = onewire_validate_pin(pin);
        if (err != ESP_OK) {
            return err;
        }
    }

    xSemaphoreTake(onewire_mutex, portMAX_DELAY);
    onewire_bus_handle_t bus = NULL;
    err = onewire_open(pin, &bus);
    if (err == ESP_OK) {
        err = onewire_bus_reset(bus);
    }
    if (err == ESP_OK && tx_len > 0) {
        err = onewire_bus_write_bytes(bus, tx_data, (uint8_t)tx_len);
    }
    if (err == ESP_OK && rx_len > 0) {
        err = onewire_bus_read_bytes(bus, rx_data, rx_len);
    }
    if (bus != NULL) {
        err = onewire_close(pin, restore_runtime_pin, bus, err);
    }
    xSemaphoreGive(onewire_mutex);
    return err;
}

esp_err_t solar_os_onewire_transfer(int pin,
                                    const uint8_t *tx_data,
                                    size_t tx_len,
                                    uint8_t *rx_data,
                                    size_t rx_len)
{
    return onewire_transfer(pin, true, tx_data, tx_len, rx_data, rx_len);
}

esp_err_t solar_os_onewire_transfer_configured(int pin,
                                               const uint8_t *tx_data,
                                               size_t tx_len,
                                               uint8_t *rx_data,
                                               size_t rx_len)
{
    return onewire_transfer(pin, false, tx_data, tx_len, rx_data, rx_len);
}

esp_err_t solar_os_onewire_bus_reset(const char *name, bool *present)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    esp_err_t ret = solar_os_bus_acquire(name,
                                         SOLAR_OS_BUS_PROTOCOL_ONEWIRE,
                                         ONEWIRE_SERVICE_OWNER);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = solar_os_bus_onewire_reset(name, present);
    return onewire_finish_operation(name, ret);
#else
    (void)name;
    if (present == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *present = false;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_onewire_bus_scan(const char *name,
                                    uint64_t *addresses,
                                    size_t max_addresses,
                                    size_t *address_count)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    esp_err_t ret = solar_os_bus_acquire(name,
                                         SOLAR_OS_BUS_PROTOCOL_ONEWIRE,
                                         ONEWIRE_SERVICE_OWNER);
    if (ret != ESP_OK) {
        if (address_count != NULL) {
            *address_count = 0;
        }
        return ret;
    }
    ret = solar_os_bus_onewire_scan(name,
                                    addresses,
                                    max_addresses,
                                    address_count);
    return onewire_finish_operation(name, ret);
#else
    (void)name;
    (void)addresses;
    (void)max_addresses;
    if (address_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *address_count = 0;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_onewire_bus_transfer(const char *name,
                                        const uint8_t *tx_data,
                                        size_t tx_len,
                                        uint8_t *rx_data,
                                        size_t rx_len)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    esp_err_t ret = solar_os_bus_acquire(name,
                                         SOLAR_OS_BUS_PROTOCOL_ONEWIRE,
                                         ONEWIRE_SERVICE_OWNER);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = solar_os_bus_onewire_transfer(name,
                                        tx_data,
                                        tx_len,
                                        rx_data,
                                        rx_len);
    return onewire_finish_operation(name, ret);
#else
    (void)name;
    (void)tx_data;
    (void)tx_len;
    (void)rx_data;
    (void)rx_len;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
