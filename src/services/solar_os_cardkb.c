#include "solar_os_cardkb.h"

#include "solar_os_board_caps.h"
#include "solar_os_log.h"

#if SOLAR_OS_BOARD_HAS_CARDKB
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus_port_a.h"
#include "solar_os_board.h"
#include "solar_os_keys.h"

#if defined(SOLAR_OS_BOARD_M5STACK_CORES3)
#include "io_expander_aw9523b.h"
#elif defined(SOLAR_OS_BOARD_M5STACK_CORE2)
#include "pmic_axp192.h"
#endif

/* M5Stack CardKB Unit: fixed I2C address, single-byte polled read with
 * no register prefix (Wire.requestFrom(addr, 1) on the reference
 * firmware) -- 0 means no key, any other value is the ASCII code of
 * the last pressed key, already cleared by the CardKB's own MCU after
 * being read, so no debounce/repeat handling is needed here.
 *
 * CardKB lives on Grove Port A, a distinct I2C controller/pins from
 * the internal bus -- see i2c_bus_port_a.c, shared with anything else
 * connected to Port A. */
#define CARDKB_I2C_ADDR 0x5f
#define CARDKB_XFER_TIMEOUT_MS 100
/* CardKB's MCU is a software/bit-banged i2c slave, not dedicated i2c
 * silicon -- standard 100kHz can outrun it. Slow way down for
 * compatibility (the reference Arduino firmware runs over the legacy,
 * more lenient Wire driver, not this newer strict i2c_master one). */
#define CARDKB_I2C_SPEED_HZ 20000U

/* CardKB's dedicated arrow keys send these fixed codes (per M5Stack's
 * CardKB documentation) instead of an ANSI escape sequence -- translate
 * them to SolarOS's own key constants so arrow-driven apps (games,
 * menus) work the same as with any other input source. Everything else
 * CardKB sends (Esc=0x1b, Backspace=0x08, Enter=0x0d, Space=0x20,
 * Tab=0x09, printable ASCII) already matches what SolarOS expects, so
 * no other translation is needed. */
#define CARDKB_KEY_LEFT 0xb4
#define CARDKB_KEY_UP 0xb5
#define CARDKB_KEY_DOWN 0xb6
#define CARDKB_KEY_RIGHT 0xb7

static const char *TAG = "solar_os_cardkb";
static i2c_master_dev_handle_t cardkb_dev_handle;
static bool cardkb_initialized;

static uint8_t cardkb_translate_key(uint8_t value)
{
    switch (value) {
        case CARDKB_KEY_LEFT: return SOLAR_OS_KEY_LEFT;
        case CARDKB_KEY_UP: return SOLAR_OS_KEY_UP;
        case CARDKB_KEY_DOWN: return SOLAR_OS_KEY_DOWN;
        case CARDKB_KEY_RIGHT: return SOLAR_OS_KEY_RIGHT;
        default: return value;
    }
}
#endif

esp_err_t solar_os_cardkb_init(void)
{
#if !SOLAR_OS_BOARD_HAS_CARDKB
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (cardkb_initialized) {
        return ESP_OK;
    }

#if defined(SOLAR_OS_BOARD_M5STACK_CORES3)
    /* CardKB is expected on Grove Port A, whose 5V rail is off by
     * default on this board (see io_expander_aw9523b.h). */
    esp_err_t ret = io_expander_aw9523b_set_grove_a_boost_enable(true);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Port A boost enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
    /* Boost is normally already on since expander init (see
     * io_expander_aw9523b.c), making this a no-op -- but if this is
     * the first moment power comes up, CardKB's MCU needs a few
     * hundred ms to boot before it'll answer on i2c (100ms was not
     * enough: the probe below NACK'd every boot until power was
     * enabled earlier/longer, while a 300ms-delay Arduino test on the
     * same unit worked). */
    vTaskDelay(pdMS_TO_TICKS(300));
#elif defined(SOLAR_OS_BOARD_M5STACK_CORE2)
    /* Grove Port A's 5V pin is gated by AXP192 GPIO0, off by default
     * (see pmic_axp192.h) -- not a fixed rail like the board header
     * comment used to (incorrectly) claim. */
    esp_err_t ret = pmic_axp192_set_grove_a_power(true);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Port A power enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
#else
    esp_err_t ret;
#endif

    ret = i2c_bus_port_a_init();
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Port A i2c bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    i2c_master_bus_handle_t cardkb_bus_handle = i2c_bus_port_a_get_handle();
    if (cardkb_bus_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_bus_port_a_lock();

    if (cardkb_dev_handle == NULL) {
        i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = CARDKB_I2C_ADDR,
            .scl_speed_hz = CARDKB_I2C_SPEED_HZ,
        };
        ret = i2c_master_bus_add_device(cardkb_bus_handle, &dev_config, &cardkb_dev_handle);
        if (ret != ESP_OK) {
            i2c_bus_port_a_unlock();
            SOLAR_OS_LOGW(TAG, "Port A i2c device add failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    /* Single plain read, exactly like the reference firmware's
     * Wire.requestFrom(addr, 1) -- no probe/scan traffic beforehand,
     * since CardKB's software i2c slave doesn't tolerate a write-
     * addressed probe followed by an early STOP (confirmed by testing:
     * it wedges the slave until it's power-cycled). */
    uint8_t probe_value = 0;
    ret = i2c_master_receive(cardkb_dev_handle, &probe_value, 1, CARDKB_XFER_TIMEOUT_MS);
    i2c_bus_port_a_unlock();
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "CardKB not found at 0x5f on Port A: %s", esp_err_to_name(ret));
        return ret;
    }

    cardkb_initialized = true;
    SOLAR_OS_LOGI(TAG, "CardKB ready");
    return ESP_OK;
#endif
}

size_t solar_os_cardkb_read_chars(char *buffer, size_t buffer_len)
{
#if !SOLAR_OS_BOARD_HAS_CARDKB
    (void)buffer;
    (void)buffer_len;
    return 0;
#else
    if (buffer == NULL || buffer_len == 0 || !cardkb_initialized) {
        return 0;
    }

    uint8_t value = 0;
    i2c_bus_port_a_lock();
    const esp_err_t ret = i2c_master_receive(cardkb_dev_handle, &value, 1, CARDKB_XFER_TIMEOUT_MS);
    i2c_bus_port_a_unlock();
    if (ret != ESP_OK || value == 0) {
        return 0;
    }

    buffer[0] = (char)cardkb_translate_key(value);
    return 1;
#endif
}
