#include "i2c_bus.h"

#include <inttypes.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_board.h"

#define I2C_XFER_TIMEOUT_MS 100

static const char *TAG = "i2c_bus";

static i2c_master_bus_handle_t bus_handle;
static SemaphoreHandle_t bus_mutex;
static i2c_bus_config_t active_config;
/* Only controllers created here can be reused: IDF exposes no pin/config
 * query for an independently created controller. */
static struct {
    i2c_master_bus_handle_t handle;
    i2c_bus_config_t config;
} controllers[I2C_NUM_MAX];

static bool i2c_bus_config_valid(const i2c_bus_config_t *config)
{
    return config != NULL &&
        config->port >= 0 &&
        config->port < I2C_NUM_MAX &&
        GPIO_IS_VALID_GPIO(config->sda_pin) &&
        GPIO_IS_VALID_GPIO(config->scl_pin) &&
        config->sda_pin != config->scl_pin &&
        config->speed_hz > 0;
}

static bool i2c_bus_config_equal(const i2c_bus_config_t *left,
                                 const i2c_bus_config_t *right)
{
    return left != NULL && right != NULL &&
        left->port == right->port &&
        left->sda_pin == right->sda_pin &&
        left->scl_pin == right->scl_pin &&
        left->speed_hz == right->speed_hz;
}

static esp_err_t i2c_bus_ensure_mutex(void)
{
    if (bus_mutex == NULL) {
        bus_mutex = xSemaphoreCreateMutex();
    }
    return bus_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t i2c_bus_device(i2c_master_bus_handle_t handle,
                                uint32_t speed_hz,
                                uint8_t address,
                                i2c_master_dev_handle_t *dev_handle)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = speed_hz,
    };

    return i2c_master_bus_add_device(handle, &dev_config, dev_handle);
}

static esp_err_t i2c_bus_start_config_locked(const i2c_bus_config_t *config,
                                             bool allow_existing,
                                             i2c_master_bus_handle_t *handle,
                                             bool *initialized_here)
{
    if (controllers[config->port].handle != NULL) {
        if (!allow_existing ||
            !i2c_bus_config_equal(config, &controllers[config->port].config)) {
            return ESP_ERR_INVALID_STATE;
        }
        *handle = controllers[config->port].handle;
        *initialized_here = false;
        return ESP_OK;
    }
    i2c_master_bus_config_t bus_config = {
        .i2c_port = config->port,
        .sda_io_num = config->sda_pin,
        .scl_io_num = config->scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, handle);
    if (ret != ESP_OK) {
        *handle = NULL;
        return ret;
    }
    controllers[config->port].handle = *handle;
    controllers[config->port].config = *config;
    *initialized_here = true;
    return ESP_OK;
}

esp_err_t i2c_bus_init(void)
{
#if defined(SOLAR_OS_BOARD_I2C_PORT) && \
    defined(SOLAR_OS_BOARD_PIN_I2C_SDA) && \
    defined(SOLAR_OS_BOARD_PIN_I2C_SCL)
    const i2c_bus_config_t config = {
        .port = SOLAR_OS_BOARD_I2C_PORT,
        .sda_pin = SOLAR_OS_BOARD_PIN_I2C_SDA,
        .scl_pin = SOLAR_OS_BOARD_PIN_I2C_SCL,
        .speed_hz = SOLAR_I2C_SPEED_HZ,
    };
    return i2c_bus_init_config(&config);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t i2c_bus_init_config(const i2c_bus_config_t *config)
{
    if (!i2c_bus_config_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(i2c_bus_ensure_mutex(), TAG, "create I2C mutex failed");

    xSemaphoreTake(bus_mutex, portMAX_DELAY);
    if (bus_handle != NULL) {
        const esp_err_t ret = i2c_bus_config_equal(config, &active_config)
            ? ESP_OK
            : ESP_ERR_INVALID_STATE;
        xSemaphoreGive(bus_mutex);
        return ret;
    }

    bool initialized_here = false;
    const esp_err_t ret = i2c_bus_start_config_locked(config,
                                                       true,
                                                       &bus_handle,
                                                       &initialized_here);
    if (ret != ESP_OK) {
        xSemaphoreGive(bus_mutex);
        ESP_RETURN_ON_ERROR(ret, TAG, "new I2C bus failed");
    }
    active_config = *config;
    ESP_LOGI(TAG,
             "I2C bus ready: port=%d SDA=%d SCL=%d speed=%" PRIu32,
             config->port,
             config->sda_pin,
             config->scl_pin,
             config->speed_hz);

    xSemaphoreGive(bus_mutex);
    return ESP_OK;
}

esp_err_t i2c_bus_start_default_config(const i2c_bus_config_t *config,
                                       i2c_master_bus_handle_t *handle,
                                       bool *initialized_here)
{
    if (!i2c_bus_config_valid(config) || handle == NULL || initialized_here == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(i2c_bus_ensure_mutex(), TAG, "create I2C mutex failed");

    *handle = NULL;
    *initialized_here = false;
    xSemaphoreTake(bus_mutex, portMAX_DELAY);
    if (bus_handle != NULL && !i2c_bus_config_equal(config, &active_config)) {
        xSemaphoreGive(bus_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t ret = i2c_bus_start_config_locked(config, true, handle, initialized_here);
    if (ret == ESP_OK) {
        bus_handle = *handle;
        active_config = *config;
    }
    xSemaphoreGive(bus_mutex);
    return ret;
}

esp_err_t i2c_bus_start_config(const i2c_bus_config_t *config,
                               bool allow_existing,
                               i2c_master_bus_handle_t *handle,
                               bool *initialized_here)
{
    if (!i2c_bus_config_valid(config) || handle == NULL || initialized_here == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = i2c_bus_ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    *handle = NULL;
    *initialized_here = false;
    xSemaphoreTake(bus_mutex, portMAX_DELAY);
    ret = i2c_bus_start_config_locked(config,
                                      allow_existing,
                                      handle,
                                      initialized_here);
    xSemaphoreGive(bus_mutex);
    return ret;
}

esp_err_t i2c_bus_stop_config(const i2c_bus_config_t *config,
                              i2c_master_bus_handle_t handle,
                              bool initialized_here)
{
    if (!i2c_bus_config_valid(config) || handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = i2c_bus_ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(bus_mutex, portMAX_DELAY);
    if (controllers[config->port].handle != handle ||
        !i2c_bus_config_equal(config, &controllers[config->port].config)) {
        xSemaphoreGive(bus_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (!initialized_here) {
        xSemaphoreGive(bus_mutex);
        return ESP_OK;
    }
    ret = i2c_del_master_bus(handle);
    if (ret == ESP_OK) {
        controllers[config->port].handle = NULL;
        controllers[config->port].config = (i2c_bus_config_t){0};
        if (bus_handle == handle) {
            bus_handle = NULL;
            active_config = (i2c_bus_config_t){0};
        }
        (void)gpio_reset_pin(config->sda_pin);
        (void)gpio_reset_pin(config->scl_pin);
    }
    xSemaphoreGive(bus_mutex);
    return ret;
}

esp_err_t i2c_bus_set_speed(i2c_master_bus_handle_t handle,
                            uint32_t speed_hz)
{
    if (handle == NULL || speed_hz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = i2c_bus_ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    /* ESP-IDF applies the clock to each device transaction. Keep the board
     * bus compatibility path in sync without invalidating the master handle. */
    xSemaphoreTake(bus_mutex, portMAX_DELAY);
    for (size_t port = 0; port < I2C_NUM_MAX; port++) {
        if (controllers[port].handle == handle) {
            controllers[port].config.speed_hz = speed_hz;
        }
    }
    if (handle == bus_handle) {
        active_config.speed_hz = speed_hz;
    }
    xSemaphoreGive(bus_mutex);
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_bus_get_handle(void)
{
    return bus_handle;
}

void i2c_bus_lock(void)
{
    if (bus_mutex != NULL) {
        xSemaphoreTake(bus_mutex, portMAX_DELAY);
    }
}

void i2c_bus_unlock(void)
{
    if (bus_mutex != NULL) {
        xSemaphoreGive(bus_mutex);
    }
}

uint32_t i2c_bus_get_speed_hz(void)
{
    return bus_handle != NULL ? active_config.speed_hz : SOLAR_I2C_SPEED_HZ;
}

gpio_num_t i2c_bus_get_sda_pin(void)
{
    if (bus_handle != NULL) {
        return active_config.sda_pin;
    }
#ifdef SOLAR_OS_BOARD_PIN_I2C_SDA
    return SOLAR_OS_BOARD_PIN_I2C_SDA;
#else
    return GPIO_NUM_NC;
#endif
}

gpio_num_t i2c_bus_get_scl_pin(void)
{
    if (bus_handle != NULL) {
        return active_config.scl_pin;
    }
#ifdef SOLAR_OS_BOARD_PIN_I2C_SCL
    return SOLAR_OS_BOARD_PIN_I2C_SCL;
#else
    return GPIO_NUM_NC;
#endif
}

static esp_err_t i2c_bus_probe_impl(i2c_master_bus_handle_t handle,
                                    uint8_t address,
                                    bool default_bus)
{
    if ((!default_bus && handle == NULL) || address > 0x7fU) {
        return address > 0x7fU ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = i2c_bus_ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(bus_mutex, portMAX_DELAY);

    if (default_bus) {
        handle = bus_handle;
        if (handle == NULL) {
            xSemaphoreGive(bus_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }
    ret = i2c_master_probe(handle, address, I2C_XFER_TIMEOUT_MS);
    xSemaphoreGive(bus_mutex);
    return ret;
}

esp_err_t i2c_bus_probe(uint8_t address)
{
    return i2c_bus_probe_impl(NULL, address, true);
}

esp_err_t i2c_bus_probe_handle(i2c_master_bus_handle_t handle, uint8_t address)
{
    return i2c_bus_probe_impl(handle, address, false);
}

static esp_err_t i2c_bus_transmit_impl(i2c_master_bus_handle_t handle,
                                       uint32_t speed_hz,
                                       uint8_t address,
                                       const uint8_t *data,
                                       size_t len,
                                       bool default_bus)
{
    if ((!default_bus && (handle == NULL || speed_hz == 0)) || address > 0x7fU ||
        data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = i2c_bus_ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }
    xSemaphoreTake(bus_mutex, portMAX_DELAY);

    if (default_bus) {
        handle = bus_handle;
        speed_hz = active_config.speed_hz;
        if (handle == NULL) {
            xSemaphoreGive(bus_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }
    i2c_master_dev_handle_t dev_handle;
    ret = i2c_bus_device(handle, speed_hz, address, &dev_handle);
    if (ret == ESP_OK) {
        ret = i2c_master_transmit(dev_handle, data, len, I2C_XFER_TIMEOUT_MS);
        const esp_err_t rm_ret = i2c_master_bus_rm_device(dev_handle);
        if (ret == ESP_OK) {
            ret = rm_ret;
        }
    }
    xSemaphoreGive(bus_mutex);
    return ret;
}

esp_err_t i2c_bus_transmit(uint8_t address, const uint8_t *data, size_t len)
{
    return i2c_bus_transmit_impl(NULL, 0, address, data, len, true);
}

esp_err_t i2c_bus_transmit_handle(i2c_master_bus_handle_t handle,
                                  uint32_t speed_hz,
                                  uint8_t address,
                                  const uint8_t *data,
                                  size_t len)
{
    return i2c_bus_transmit_impl(handle, speed_hz, address, data, len, false);
}

static esp_err_t i2c_bus_receive_impl(i2c_master_bus_handle_t handle,
                                      uint32_t speed_hz,
                                      uint8_t address,
                                      uint8_t *data,
                                      size_t len,
                                      bool default_bus)
{
    if ((!default_bus && (handle == NULL || speed_hz == 0)) || address > 0x7fU ||
        data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = i2c_bus_ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(bus_mutex, portMAX_DELAY);

    if (default_bus) {
        handle = bus_handle;
        speed_hz = active_config.speed_hz;
        if (handle == NULL) {
            xSemaphoreGive(bus_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }

    i2c_master_dev_handle_t dev_handle;
    ret = i2c_bus_device(handle, speed_hz, address, &dev_handle);
    if (ret == ESP_OK) {
        ret = i2c_master_receive(dev_handle, data, len, I2C_XFER_TIMEOUT_MS);
        esp_err_t rm_ret = i2c_master_bus_rm_device(dev_handle);
        if (ret == ESP_OK) {
            ret = rm_ret;
        }
    }

    xSemaphoreGive(bus_mutex);
    return ret;
}

esp_err_t i2c_bus_receive(uint8_t address, uint8_t *data, size_t len)
{
    return i2c_bus_receive_impl(NULL, 0, address, data, len, true);
}

esp_err_t i2c_bus_receive_handle(i2c_master_bus_handle_t handle,
                                 uint32_t speed_hz,
                                 uint8_t address,
                                 uint8_t *data,
                                 size_t len)
{
    return i2c_bus_receive_impl(handle, speed_hz, address, data, len, false);
}

static esp_err_t i2c_bus_transmit_receive_impl(i2c_master_bus_handle_t handle,
                                               uint32_t speed_hz,
                                               uint8_t address,
                                               const uint8_t *tx_data,
                                               size_t tx_len,
                                               uint8_t *rx_data,
                                               size_t rx_len,
                                               bool default_bus)
{
    if ((!default_bus && (handle == NULL || speed_hz == 0)) || address > 0x7fU ||
        tx_data == NULL || tx_len == 0 || rx_data == NULL || rx_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = i2c_bus_ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }
    xSemaphoreTake(bus_mutex, portMAX_DELAY);

    if (default_bus) {
        handle = bus_handle;
        speed_hz = active_config.speed_hz;
        if (handle == NULL) {
            xSemaphoreGive(bus_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }
    i2c_master_dev_handle_t dev_handle;
    ret = i2c_bus_device(handle, speed_hz, address, &dev_handle);
    if (ret == ESP_OK) {
        ret = i2c_master_transmit_receive(dev_handle,
                                          tx_data,
                                          tx_len,
                                          rx_data,
                                          rx_len,
                                          I2C_XFER_TIMEOUT_MS);
        const esp_err_t rm_ret = i2c_master_bus_rm_device(dev_handle);
        if (ret == ESP_OK) {
            ret = rm_ret;
        }
    }
    xSemaphoreGive(bus_mutex);
    return ret;
}

esp_err_t i2c_bus_transmit_receive(uint8_t address,
                                   const uint8_t *tx_data,
                                   size_t tx_len,
                                   uint8_t *rx_data,
                                   size_t rx_len)
{
    return i2c_bus_transmit_receive_impl(NULL, 0, address, tx_data, tx_len, rx_data, rx_len, true);
}

esp_err_t i2c_bus_transmit_receive_handle(i2c_master_bus_handle_t handle,
                                          uint32_t speed_hz,
                                          uint8_t address,
                                          const uint8_t *tx_data,
                                          size_t tx_len,
                                          uint8_t *rx_data,
                                          size_t rx_len)
{
    return i2c_bus_transmit_receive_impl(handle, speed_hz, address,
                                         tx_data, tx_len, rx_data, rx_len, false);
}

esp_err_t i2c_bus_read_reg(uint8_t address, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_bus_transmit_receive(address, &reg, 1, data, len);
}

esp_err_t i2c_bus_read_reg_handle(i2c_master_bus_handle_t handle,
                                  uint32_t speed_hz,
                                  uint8_t address,
                                  uint8_t reg,
                                  uint8_t *data,
                                  size_t len)
{
    return i2c_bus_transmit_receive_handle(handle, speed_hz, address, &reg, 1, data, len);
}

static esp_err_t i2c_bus_write_reg_impl(i2c_master_bus_handle_t handle,
                                        uint32_t speed_hz,
                                        uint8_t address,
                                        uint8_t reg,
                                        const uint8_t *data,
                                        size_t len,
                                        bool default_bus)
{
    if ((!default_bus && (handle == NULL || speed_hz == 0)) || address > 0x7fU ||
        data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = i2c_bus_ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    i2c_master_transmit_multi_buffer_info_t buffers[] = {
        {
            .write_buffer = &reg,
            .buffer_size = sizeof(reg),
        },
        {
            .write_buffer = data,
            .buffer_size = len,
        },
    };

    xSemaphoreTake(bus_mutex, portMAX_DELAY);

    if (default_bus) {
        handle = bus_handle;
        speed_hz = active_config.speed_hz;
        if (handle == NULL) {
            xSemaphoreGive(bus_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }

    i2c_master_dev_handle_t dev_handle;
    ret = i2c_bus_device(handle, speed_hz, address, &dev_handle);
    if (ret == ESP_OK) {
        ret = i2c_master_multi_buffer_transmit(dev_handle,
                                               buffers,
                                               sizeof(buffers) / sizeof(buffers[0]),
                                               I2C_XFER_TIMEOUT_MS);
        esp_err_t rm_ret = i2c_master_bus_rm_device(dev_handle);
        if (ret == ESP_OK) {
            ret = rm_ret;
        }
    }

    xSemaphoreGive(bus_mutex);
    return ret;
}

esp_err_t i2c_bus_write_reg(uint8_t address, uint8_t reg, const uint8_t *data, size_t len)
{
    return i2c_bus_write_reg_impl(NULL, 0, address, reg, data, len, true);
}

esp_err_t i2c_bus_write_reg_handle(i2c_master_bus_handle_t handle,
                                   uint32_t speed_hz,
                                   uint8_t address,
                                   uint8_t reg,
                                   const uint8_t *data,
                                   size_t len)
{
    return i2c_bus_write_reg_impl(handle, speed_hz, address, reg, data, len, false);
}
