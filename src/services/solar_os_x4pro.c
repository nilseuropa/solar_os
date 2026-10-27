#include "solar_os_x4pro.h"

#include <string.h>
#include "driver/gpio.h"
#include "epd_ssd1677.h"
#include "epd_uc8179.h"
#include "epd_uc8279.h"
#include "epd_xteink_identify.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "frontlight_pwm.h"
#include "nvs.h"
#include "solar_os_board_display.h"

#define X4PRO_NVS_NAMESPACE "x4pro"
#define X4PRO_NVS_CONTROLLER "controller"

typedef struct {
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    int latch_pin;
    epd_xteink_controller_t controller;
    union { epd_ssd1677_t ssd; epd_ultrachip_t uc; } panel;
    frontlight_pwm_t light;
    solar_os_board_display_t display;
} x4pro_device_t;

static x4pro_device_t *device;
static const char *TAG = "x4pro";

static esp_err_t load_controller(epd_xteink_controller_t *controller)
{
    *controller = EPD_XTEINK_UNKNOWN;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(X4PRO_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    uint8_t id = 0;
    err = nvs_get_u8(nvs, X4PRO_NVS_CONTROLLER, &id);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (id < EPD_XTEINK_SSD1677 || id > EPD_XTEINK_UC8279)
        return ESP_ERR_INVALID_STATE;
    *controller = (epd_xteink_controller_t)id;
    return ESP_OK;
}

static esp_err_t save_controller(epd_xteink_controller_t controller)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(X4PRO_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(nvs, X4PRO_NVS_CONTROLLER, (uint8_t)controller);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t runtime_ready(solar_os_board_display_t *display)
{
    return display && display->driver ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t resume(solar_os_board_display_t *display)
{
    if (!display || !display->driver) return ESP_ERR_INVALID_STATE;
    x4pro_device_t *d = display->driver;
    esp_err_t err = d->controller == EPD_XTEINK_SSD1677 ?
        epd_ssd1677_resume(&d->panel.ssd) : epd_ultrachip_resume(&d->panel.uc);
    if (err == ESP_OK) err = frontlight_pwm_suspend(&d->light, false);
    display->ready = err == ESP_OK;
    return err;
}

static void deinit(solar_os_board_display_t *display)
{
    if (!display || !display->driver) return;
    x4pro_device_t *d = display->driver;
    frontlight_pwm_deinit(&d->light);
    if (d->controller == EPD_XTEINK_SSD1677) epd_ssd1677_deinit(&d->panel.ssd);
    else epd_ultrachip_deinit(&d->panel.uc);
    display->ready = false;
    /* GPIO1 supplies other board peripherals; keep its latch asserted. */
}

static esp_err_t set_power_save(solar_os_board_display_t *display, bool enabled)
{
    if (!display || !display->driver) return ESP_ERR_INVALID_STATE;
    x4pro_device_t *d = display->driver;
    if (enabled) {
        esp_err_t err = frontlight_pwm_suspend(&d->light, true);
        if (err != ESP_OK) return err;
    }
    u8g2_SetPowerSave(display->u8g2, enabled ? 1 : 0);
    const esp_err_t err = d->controller == EPD_XTEINK_SSD1677 ?
        d->panel.ssd.last_error : d->panel.uc.last_error;
    if (err != ESP_OK) {
        if (enabled) (void)frontlight_pwm_suspend(&d->light, false);
        return err;
    }
    return enabled ? ESP_OK : frontlight_pwm_suspend(&d->light, false);
}

static bool brightness_supported(const solar_os_board_display_t *display)
{
    return display && display->driver && ((x4pro_device_t *)display->driver)->light.ready;
}

static esp_err_t get_brightness(const solar_os_board_display_t *display, uint8_t *percent)
{
    if (!percent) return ESP_ERR_INVALID_ARG;
    if (!brightness_supported(display)) return ESP_ERR_INVALID_STATE;
    *percent = ((x4pro_device_t *)display->driver)->light.percent;
    return ESP_OK;
}

static esp_err_t set_brightness(solar_os_board_display_t *display, uint8_t percent)
{
    return display && display->driver ?
        frontlight_pwm_set(&((x4pro_device_t *)display->driver)->light, percent) :
        ESP_ERR_INVALID_STATE;
}

static const char *controller_mode(const solar_os_board_display_t *display)
{
    if (!display || !display->driver) return NULL;
    const x4pro_device_t *d = display->driver;
    return d->controller == EPD_XTEINK_SSD1677 ?
        epd_ssd1677_controller_mode(&d->panel.ssd) : epd_ultrachip_controller_mode(&d->panel.uc);
}

static const char *controller_mode_values(const solar_os_board_display_t *display)
{
    if (!display || !display->driver) return NULL;
    const x4pro_device_t *d = display->driver;
    return d->controller == EPD_XTEINK_SSD1677 ?
        epd_ssd1677_controller_mode_values(&d->panel.ssd) :
        epd_ultrachip_controller_mode_values(&d->panel.uc);
}

static esp_err_t set_controller_mode(solar_os_board_display_t *display, const char *mode)
{
    if (!display || !display->driver) return ESP_ERR_INVALID_STATE;
    x4pro_device_t *d = display->driver;
    return d->controller == EPD_XTEINK_SSD1677 ?
        epd_ssd1677_set_controller_mode(&d->panel.ssd, mode) :
        epd_ultrachip_set_controller_mode(&d->panel.uc, mode);
}

static esp_err_t relax(solar_os_board_display_t *display, solar_os_display_relax_phase_t phase)
{
    if (!display || !display->driver) return ESP_ERR_INVALID_STATE;
    x4pro_device_t *d = display->driver;
    return d->controller == EPD_XTEINK_SSD1677 ?
        epd_ssd1677_relax(&d->panel.ssd, phase) : epd_ultrachip_relax(&d->panel.uc, phase);
}

static esp_err_t shutdown(solar_os_board_display_t *display)
{
    if (display == NULL || display->driver == NULL) return ESP_ERR_INVALID_STATE;
    x4pro_device_t *d = display->driver;
    const esp_err_t err = gpio_set_level(d->latch_pin, 0);
    if (err == ESP_OK) {
        const esp_err_t hold_err = gpio_hold_en(d->latch_pin);
        if (hold_err != ESP_OK) return hold_err;
        gpio_deep_sleep_hold_en();
    }
    return err;
}

static const solar_os_board_display_ops_t ops = {
    .runtime_ready = runtime_ready, .resume = resume, .deinit = deinit,
    .set_power_save = set_power_save,
    .shutdown = shutdown,
    .brightness_supported = brightness_supported,
    .get_brightness = get_brightness, .set_brightness = set_brightness,
    .controller_mode = controller_mode, .controller_mode_values = controller_mode_values,
    .set_controller_mode = set_controller_mode,
    .relax = relax,
};

esp_err_t solar_os_x4pro_attach(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t binding_count)
{
    static const char *const roles[] = {"cs", "dc", "reset", "busy", "latch", "cool", "warm"};
    int pins[7] = {-1, -1, -1, -1, -1, -1, -1};
    const char *spi_bus = NULL;
    if (!name || !name[0] || !bindings || device ||
        strlen(name) >= SOLAR_OS_EXPANSION_DEVICE_NAME_MAX) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < binding_count; ++i) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS && !spi_bus)
            spi_bus = b->target;
        else if (b->kind == SOLAR_OS_EXPANSION_BINDING_SPI_CS && pins[0] < 0)
            pins[0] = b->value;
        else if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO) {
            size_t role = 1;
            while (role < 7 && strcmp(b->role, roles[role])) ++role;
            if (role == 7 || pins[role] >= 0) return ESP_ERR_INVALID_ARG;
            pins[role] = b->value;
        } else return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < 7; ++i) {
        if (i == 3 ? !GPIO_IS_VALID_GPIO(pins[i]) : !GPIO_IS_VALID_OUTPUT_GPIO(pins[i]))
            return ESP_ERR_INVALID_ARG;
        for (size_t j = 0; j < i; ++j)
            if (pins[i] == pins[j]) return ESP_ERR_INVALID_ARG;
    }
    solar_os_expansion_spi_bus_t bus;
    if (!spi_bus || !solar_os_expansion_find_spi_bus(spi_bus, &bus, NULL) ||
        !solar_os_expansion_spi_cs_allowed(spi_bus, pins[0])) return ESP_ERR_INVALID_ARG;
    x4pro_device_t *d = heap_caps_calloc(1, sizeof(*d), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!d) return ESP_ERR_NO_MEM;
    strlcpy(d->name, name, sizeof(d->name));
    d->display.ops = &ops;
    d->display.driver = d;
    const gpio_config_t latch = {.pin_bit_mask = 1ULL << pins[4], .mode = GPIO_MODE_OUTPUT};
    d->latch_pin = pins[4];
    (void)gpio_hold_dis(pins[4]);
    esp_err_t err = gpio_config(&latch);
    if (err == ESP_OK) err = gpio_set_level(pins[4], 1);
    if (err == ESP_OK) err = frontlight_pwm_init(&d->light, pins[5], pins[6]);
    if (err == ESP_OK) err = load_controller(&d->controller);
    const bool identify = d->controller == EPD_XTEINK_UNKNOWN;
    if (err == ESP_OK && identify)
        err = epd_xteink_identify(spi_bus, pins[0], pins[1], pins[2], bus.mosi_pin, &d->controller);
    if (err == ESP_OK && d->controller == EPD_XTEINK_SSD1677) {
        const epd_ssd1677_config_t config = {
            .spi_bus = spi_bus, .cs_pin = pins[0], .dc_pin = pins[1],
            .reset_pin = pins[2], .busy_pin = pins[3], .power_pin = -1,
            .spi_clock_hz = 10000000, .busy_level = 1, .power_active_level = 1,
            .rotation = U8G2_R0,
        };
        err = epd_ssd1677_init(&d->panel.ssd, &config);
        d->display.u8g2 = epd_ssd1677_get_u8g2(&d->panel.ssd);
        d->display.controller = "SSD1677";
    } else if (err == ESP_OK) {
        const epd_ultrachip_config_t config = {
            .spi_bus = spi_bus, .cs_pin = pins[0], .dc_pin = pins[1],
            .reset_pin = pins[2], .busy_pin = pins[3], .power_pin = -1,
            .spi_clock_hz = 10000000, .rotation = U8G2_R0, .panel = 1,
        };
        err = d->controller == EPD_XTEINK_UC8179 ?
            epd_uc8179_init(&d->panel.uc, &config) : epd_uc8279_init(&d->panel.uc, &config);
        d->display.u8g2 = epd_ultrachip_get_u8g2(&d->panel.uc);
        d->display.controller = d->controller == EPD_XTEINK_UC8179 ? "UC8179" : "UC8279";
    }
    d->display.driver_name = "x4pro";
    d->display.width = 800;
    d->display.height = 480;
    d->display.ready = err == ESP_OK;
    if (err == ESP_OK) err = solar_os_board_display_register_primary(&d->display);
    if (err != ESP_OK) {
        deinit(&d->display);
        heap_caps_free(d);
        return err;
    }
    if (identify) {
        /* Successful initialization precedes persistence. A write failure keeps
         * the running display usable and causes identification on the next boot. */
        const esp_err_t saved = save_controller(d->controller);
        if (saved != ESP_OK) ESP_LOGW(TAG, "Controller cache unavailable: %s", esp_err_to_name(saved));
    }
    ESP_LOGI(TAG, "%s controller (%s)", d->display.controller, identify ? "identified" : "NVS");
    device = d;
    return ESP_OK;
}

esp_err_t solar_os_x4pro_detach(const char *name)
{
    if (!device || !name || strcmp(device->name, name)) return ESP_ERR_NOT_FOUND;
    /* The primary display service owns the lifetime, like other primary panels. */
    return ESP_ERR_INVALID_STATE;
}
