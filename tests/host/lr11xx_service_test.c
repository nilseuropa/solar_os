#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../../src/services/solar_os_lr11xx.c"

/*
 * The expansion wrapper around the LR11xx driver, with the driver and the
 * radio registry faked: what is tested is what the wrapper does when one of
 * them refuses, which is where a radio ends up attached, asleep, both or
 * neither.
 */

static esp_err_t init_result;
static esp_err_t probe_result;
static esp_err_t configure_result;
static esp_err_t sleep_result;
static esp_err_t register_result;
static esp_err_t unregister_result;
static unsigned sleeps;
static bool registered;
static solar_os_radio_state_t registered_state;

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "err";
}

esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...)
{
    (void)level; (void)tag; (void)fmt;
    return ESP_OK;
}

int gpio_get_level(gpio_num_t pin)
{
    (void)pin;
    return 0;
}

esp_err_t lr11xx_init(lr11xx_t *dev, const char *spi_bus, int cs_pin, int busy_pin,
                      int reset_pin, int irq_pin, uint32_t speed_hz, uint16_t tcxo_mv,
                      const lr11xx_rf_switch_t *rf_switch)
{
    (void)spi_bus; (void)cs_pin; (void)busy_pin; (void)reset_pin; (void)irq_pin;
    (void)speed_hz; (void)tcxo_mv; (void)rf_switch;
    memset(dev, 0, sizeof(*dev));
    return init_result;
}

esp_err_t lr11xx_probe(lr11xx_t *dev, lr11xx_version_t *version)
{
    (void)dev;
    version->part = LR11XX_PART_LR1110;
    version->device_code = 0x01U;
    return probe_result;
}

esp_err_t lr11xx_configure(lr11xx_t *dev, const solar_os_radio_config_t *config)
{
    if (configure_result == ESP_OK) {
        dev->config = *config;
        dev->state = SOLAR_OS_RADIO_STATE_STANDBY;
    }
    return configure_result;
}

esp_err_t lr11xx_set_state(lr11xx_t *dev, solar_os_radio_state_t state)
{
    if (state == SOLAR_OS_RADIO_STATE_SLEEP) {
        sleeps++;
        dev->state = sleep_result == ESP_OK ? SOLAR_OS_RADIO_STATE_SLEEP
                                            : SOLAR_OS_RADIO_STATE_UNKNOWN;
        return sleep_result;
    }
    dev->state = state;
    return ESP_OK;
}

esp_err_t lr11xx_get_errors(lr11xx_t *dev, uint16_t *errors)
{
    (void)dev;
    *errors = 0U;
    return ESP_OK;
}

esp_err_t lr11xx_get_status(lr11xx_t *dev, solar_os_radio_status_t *status)
{
    (void)dev; (void)status;
    return ESP_OK;
}

esp_err_t lr11xx_send(lr11xx_t *dev, const solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    (void)dev; (void)packet; (void)timeout_ms;
    return ESP_OK;
}

esp_err_t lr11xx_send_stream(lr11xx_t *dev, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    (void)dev; (void)data; (void)len; (void)timeout_ms;
    return ESP_OK;
}

esp_err_t lr11xx_receive(lr11xx_t *dev, solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    (void)dev; (void)packet; (void)timeout_ms;
    return ESP_OK;
}

const char *lr11xx_part_name(lr11xx_part_t part)
{
    (void)part;
    return "LR1110";
}

esp_err_t solar_os_radio_register(const solar_os_radio_registration_t *registration)
{
    if (register_result == ESP_OK) {
        registered = true;
        registered_state = registration->initial_state;
    }
    return register_result;
}

esp_err_t solar_os_radio_unregister(const char *name)
{
    (void)name;
    if (unregister_result == ESP_OK) {
        registered = false;
    }
    return unregister_result;
}

bool solar_os_expansion_find_spi_bus(const char *name, solar_os_expansion_spi_bus_t *bus,
                                     size_t *index)
{
    (void)bus; (void)index;
    return strcmp(name, "spi0") == 0;
}

bool solar_os_expansion_spi_cs_allowed(const char *bus, int cs_pin)
{
    (void)bus;
    return cs_pin == 39;
}

static const solar_os_expansion_binding_t bindings[] = {
    {.kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .target = "spi0"},
    {.kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .value = 39},
    {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "busy", .value = 41},
};

static void reset(void)
{
    for (size_t i = 0; i < SOLAR_OS_LR11XX_MAX; i++) {
        clear_device(&devices[i]);
    }
    init_result = probe_result = configure_result = ESP_OK;
    sleep_result = register_result = unregister_result = ESP_OK;
    sleeps = 0U;
    registered = false;
}

static esp_err_t attach(void)
{
    return solar_os_lr11xx_attach("radio0", bindings, sizeof(bindings) / sizeof(bindings[0]));
}

static void test_attach_and_detach(void)
{
    reset();
    assert(attach() == ESP_OK);
    assert(registered && registered_state == SOLAR_OS_RADIO_STATE_STANDBY);
    assert(sleeps == 0U);
    assert(solar_os_lr11xx_detach("radio0") == ESP_OK);
    assert(!registered && sleeps == 1U);
    /* The slot is free again. */
    assert(attach() == ESP_OK);
}

/* A part that answered and was then not attached is not left awake. */
static void test_failed_attach_sleeps_the_part(void)
{
    reset();
    configure_result = ESP_FAIL;
    assert(attach() == ESP_FAIL);
    assert(!registered && sleeps == 1U);

    reset();
    register_result = ESP_ERR_NO_MEM;
    assert(attach() == ESP_ERR_NO_MEM);
    assert(!registered && sleeps == 1U);

    /* One that never answered is not spoken to again. */
    reset();
    probe_result = ESP_ERR_NOT_FOUND;
    assert(attach() == ESP_ERR_NOT_FOUND);
    assert(sleeps == 0U);
    reset();
    init_result = ESP_ERR_TIMEOUT;
    assert(attach() == ESP_ERR_TIMEOUT);
    assert(sleeps == 0U);
    /* A failed attach does not hold the slot. */
    init_result = ESP_OK;
    assert(attach() == ESP_OK);
}

/* A radio that will not sleep is let go all the same: a part that is dead
 * or gone never will, and must not hold its name until a reboot. */
static void test_detach_lets_go_of_a_radio_that_will_not_sleep(void)
{
    reset();
    assert(attach() == ESP_OK);
    sleep_result = ESP_FAIL;
    assert(solar_os_lr11xx_detach("radio0") == ESP_OK);
    assert(!registered && sleeps == 1U);

    sleep_result = ESP_OK;
    assert(attach() == ESP_OK); /* the name is free again */
}

/* A radio somebody holds is neither let go nor put to sleep under them. */
static void test_detach_leaves_a_radio_in_use(void)
{
    reset();
    assert(attach() == ESP_OK);
    unregister_result = ESP_ERR_INVALID_STATE;
    assert(solar_os_lr11xx_detach("radio0") == ESP_ERR_INVALID_STATE);
    assert(registered && sleeps == 0U);
}

int main(void)
{
    test_attach_and_detach();
    test_failed_attach_sleeps_the_part();
    test_detach_lets_go_of_a_radio_that_will_not_sleep();
    test_detach_leaves_a_radio_in_use();
    puts("lr11xx service tests: ok");
    return 0;
}
