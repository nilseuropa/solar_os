#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "freertos/semphr.h"
#include "i2c_bus.h"
#include "routed_spi_bus.h"
#include "solar_os_buses.h"
#include "solar_os_resources.h"

static struct {
    bool allocated;
    i2c_master_bus_config_t config;
} hardware[I2C_NUM_MAX];
static bool fail_delete;
static unsigned creates;
static unsigned deletes;
static unsigned pin_resets;
static int last_port;
static uint32_t last_speed;
static void (*before_lock)(void);
static const i2c_bus_config_t primary = {0, 8, 9, 100000};

void i2c_test_before_lock(void)
{
    if (before_lock != NULL) {
        void (*hook)(void) = before_lock;
        before_lock = NULL;
        hook();
    }
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static StaticSemaphore_t storage;
    return &storage;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size != 0) {
        const size_t copy = len < size - 1 ? len : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    const size_t used = strnlen(dst, size);
    return used + strlcpy(dst + used, src, size - used);
}

bool solar_os_pin_is_routable(int pin)
{
    return GPIO_IS_VALID_GPIO(pin);
}

esp_err_t gpio_reset_pin(gpio_num_t pin)
{
    assert(GPIO_IS_VALID_GPIO(pin));
    pin_resets++;
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t pin, int level)
{
    (void)pin;
    (void)level;
    assert(false); /* This fixture contains only I2C board buses. */
    return ESP_FAIL;
}

esp_err_t gpio_set_direction(gpio_num_t pin, int mode)
{
    (void)pin;
    (void)mode;
    assert(false);
    return ESP_FAIL;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config,
                             i2c_master_bus_handle_t *handle)
{
    assert(config->i2c_port >= 0 && config->i2c_port < I2C_NUM_MAX);
    const int port = config->i2c_port;
    if (hardware[port].allocated) {
        return ESP_ERR_INVALID_STATE;
    }
    hardware[port].allocated = true;
    hardware[port].config = *config;
    *handle = &hardware[port];
    creates++;
    return ESP_OK;
}

static int checked_port(i2c_master_bus_handle_t handle)
{
    for (int port = 0; port < I2C_NUM_MAX; port++) {
        if (handle == &hardware[port]) {
            assert(hardware[port].allocated); /* Catch use after controller deletion. */
            return port;
        }
    }
    assert(false);
    return -1;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t handle)
{
    const int port = checked_port(handle);
    if (fail_delete) {
        return ESP_ERR_INVALID_STATE;
    }
    hardware[port].allocated = false;
    deletes++;
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t handle, uint8_t address, int timeout)
{
    (void)timeout;
    assert(address <= 0x7f);
    last_port = checked_port(handle);
    return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t handle,
                                    const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *device)
{
    last_port = checked_port(handle);
    last_speed = config->scl_speed_hz;
    *device = handle;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
    (void)checked_port(device);
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
                              const uint8_t *data, size_t len, int timeout)
{
    (void)timeout;
    assert(data != NULL && len != 0);
    (void)checked_port(device);
    return ESP_OK;
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t device,
                             uint8_t *data, size_t len, int timeout)
{
    (void)timeout;
    memset(data, checked_port(device), len);
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
                                     const uint8_t *tx, size_t tx_len,
                                     uint8_t *rx, size_t rx_len, int timeout)
{
    assert(i2c_master_transmit(device, tx, tx_len, timeout) == ESP_OK);
    return i2c_master_receive(device, rx, rx_len, timeout);
}

esp_err_t i2c_master_multi_buffer_transmit(i2c_master_dev_handle_t device,
                                          const i2c_master_transmit_multi_buffer_info_t *buffers,
                                          size_t count, int timeout)
{
    for (size_t i = 0; i < count; i++) {
        assert(i2c_master_transmit(device, buffers[i].write_buffer,
                                   buffers[i].buffer_size, timeout) == ESP_OK);
    }
    return ESP_OK;
}

esp_err_t solar_os_routed_spi_start(const solar_os_bus_spi_config_t *config,
                                    bool allow_existing, bool *initialized_here)
{
    (void)config;
    (void)allow_existing;
    (void)initialized_here;
    assert(false);
    return ESP_FAIL;
}

esp_err_t solar_os_routed_spi_stop(const solar_os_bus_spi_config_t *config, bool initialized_here)
{
    (void)config;
    (void)initialized_here;
    assert(false);
    return ESP_FAIL;
}

static void stop_primary(void)
{
    assert(i2c_bus_stop_config(&primary, i2c_bus_get_handle(), true) == ESP_OK);
}

static void test_board_controllers(void)
{
    assert(solar_os_buses_init() == ESP_OK);
    assert(solar_os_bus_acquire("i2c1", SOLAR_OS_BUS_PROTOCOL_I2C, "rtc") == ESP_OK);
    assert(i2c_bus_get_handle() == NULL); /* Secondary-first must not redirect legacy calls. */
    assert(solar_os_bus_acquire("i2c0", SOLAR_OS_BUS_PROTOCOL_I2C, "keypad") == ESP_OK);
    i2c_master_bus_handle_t first, second;
    assert(solar_os_bus_i2c_get_handle("i2c0", &first, NULL) == ESP_OK);
    assert(solar_os_bus_i2c_get_handle("i2c1", &second, NULL) == ESP_OK);
    assert(first != second && first == i2c_bus_get_handle());
    assert(creates == 2);
    assert(solar_os_bus_i2c_probe("i2c1", 0x51) == ESP_OK && last_port == 1);
    assert(i2c_bus_probe(0x6c) == ESP_OK && last_port == 0);
    assert(solar_os_bus_detach("i2c0") == ESP_ERR_INVALID_STATE); /* Live lease protects it. */
    assert(solar_os_bus_release("i2c0", SOLAR_OS_BUS_PROTOCOL_I2C, "keypad") == ESP_OK);
    fail_delete = true;
    const size_t claims = solar_os_resource_claim_count();
    assert(solar_os_bus_detach("i2c0") == ESP_ERR_INVALID_STATE);
    solar_os_bus_info_t info;
    assert(solar_os_bus_find("i2c0", SOLAR_OS_BUS_PROTOCOL_I2C, &info));
    assert(info.attached && info.ready && i2c_bus_get_handle() == first);
    assert(i2c_bus_probe(0x6c) == ESP_OK);
    assert(pin_resets == 0 && solar_os_resource_claim_count() == claims);
    fail_delete = false;
    assert(solar_os_bus_i2c_set_speed("i2c0", 400000) == ESP_OK);
    uint8_t data = 1;
    assert(i2c_bus_transmit(0x6c, &data, 1) == ESP_OK && last_speed == 400000);
    assert(solar_os_bus_detach("i2c0") == ESP_OK);
    assert(i2c_bus_get_handle() == NULL && pin_resets == 2 && deletes == 1);
    assert(solar_os_bus_i2c_probe("i2c1", 0x51) == ESP_OK && last_port == 1);
    assert(solar_os_bus_release("i2c1", SOLAR_OS_BUS_PROTOCOL_I2C, "rtc") == ESP_OK);
    assert(solar_os_bus_detach("i2c1") == ESP_OK);
    assert(deletes == 2 && solar_os_resource_claim_count() == 0);
}

static void test_legacy_reuse(void)
{
    assert(i2c_bus_init() == ESP_OK);
    i2c_master_bus_handle_t legacy = i2c_bus_get_handle();
    const unsigned previous_creates = creates;
    assert(solar_os_bus_i2c_set_speed("i2c0", 100000) == ESP_OK);
    assert(solar_os_bus_attach("i2c0") == ESP_OK);
    assert(solar_os_bus_acquire("i2c0", SOLAR_OS_BUS_PROTOCOL_I2C, "display") == ESP_OK);
    assert(i2c_bus_get_handle() == legacy && creates == previous_creates);
    i2c_bus_config_t mismatch = primary;
    mismatch.sda_pin = 12;
    i2c_master_bus_handle_t handle;
    bool owned;
    assert(i2c_bus_start_config(&mismatch, true, &handle, &owned) == ESP_ERR_INVALID_STATE);
    assert(handle == NULL && !owned);
    mismatch = primary;
    mismatch.speed_hz = 400000;
    assert(i2c_bus_start_config(&mismatch, true, &handle, &owned) == ESP_ERR_INVALID_STATE);
    assert(i2c_bus_start_config(&primary, false, &handle, &owned) == ESP_ERR_INVALID_STATE);
    assert(solar_os_bus_release("i2c0", SOLAR_OS_BUS_PROTOCOL_I2C, "display") == ESP_OK);
    const unsigned previous_deletes = deletes;
    assert(solar_os_bus_detach("i2c0") == ESP_OK);
    assert(deletes == previous_deletes && i2c_bus_get_handle() == legacy);
    assert(i2c_bus_probe(0x6c) == ESP_OK);
    stop_primary();
    /* Do not borrow a raw IDF controller whose wiring cannot be verified. */
    hardware[1].allocated = true;
    const i2c_bus_config_t secondary = {1, 10, 11, 100000};
    assert(i2c_bus_start_config(&secondary, true, &handle, &owned) == ESP_ERR_INVALID_STATE);
    assert(handle == NULL && !owned);
    hardware[1].allocated = false;
}

static void test_legacy_shutdown_race(void)
{
    uint8_t byte = 0;
    for (int operation = 0; operation < 6; operation++) {
        assert(i2c_bus_init() == ESP_OK);
        /* Simulate deletion by another task while this call waits for the lock. */
        before_lock = stop_primary;
        esp_err_t ret = ESP_FAIL;
        switch (operation) {
        case 0: ret = i2c_bus_probe(0x51); break;
        case 1: ret = i2c_bus_transmit(0x51, &byte, 1); break;
        case 2: ret = i2c_bus_receive(0x51, &byte, 1); break;
        case 3: ret = i2c_bus_transmit_receive(0x51, &byte, 1, &byte, 1); break;
        case 4: ret = i2c_bus_read_reg(0x51, 0, &byte, 1); break;
        case 5: ret = i2c_bus_write_reg(0x51, 0, &byte, 1); break;
        }
        assert(ret == ESP_ERR_INVALID_STATE && i2c_bus_get_handle() == NULL);
    }
}

static void test_runtime_controller(void)
{
    const solar_os_bus_definition_t definition = {
        .name = "external", .protocol = SOLAR_OS_BUS_PROTOCOL_I2C,
        .origin = SOLAR_OS_BUS_ORIGIN_RUNTIME, .sharing = SOLAR_OS_BUS_SHARED,
        .config.i2c = { .port = 1, .sda_pin = 14, .scl_pin = 15, .speed_hz = 100000 },
    };
    assert(solar_os_bus_register(&definition) == ESP_OK);
    assert(solar_os_bus_acquire("external", SOLAR_OS_BUS_PROTOCOL_I2C, "sensor") == ESP_OK);
    assert(i2c_bus_get_handle() == NULL);
    assert(solar_os_bus_i2c_probe("external", 0x51) == ESP_OK && last_port == 1);
    assert(solar_os_bus_i2c_set_speed("external", 400000) == ESP_OK);
    uint8_t byte = 42;
    assert(solar_os_bus_i2c_write_reg("external", 0x51, 0, &byte, 1) == ESP_OK);
    assert(last_port == 1 && last_speed == 400000);
    assert(solar_os_bus_i2c_read_reg("external", 0x51, 0, &byte, 1) == ESP_OK);
    assert(byte == 1);
    assert(solar_os_bus_release("external", SOLAR_OS_BUS_PROTOCOL_I2C, "sensor") == ESP_OK);
    assert(solar_os_bus_detach("external") == ESP_OK);
    assert(!hardware[1].allocated && solar_os_resource_claim_count() == 0);
    assert(solar_os_bus_unregister("external") == ESP_OK);
}

int main(void)
{
    test_board_controllers();
    test_legacy_reuse();
    test_legacy_shutdown_race();
    test_runtime_controller();
    puts("I2C controller ownership and default bus tests: ok");
    return 0;
}
