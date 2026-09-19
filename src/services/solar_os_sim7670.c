#include "solar_os_sim7670.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_gnss.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_modem.h"
#include "solar_os_ppp.h"

#define SIM7670_DEVICE_MAX 2U
#define SIM7670_PPP_READ_TIMEOUT_MS 100U
#define SIM7670_PPP_CONNECT_TIMEOUT_MS 65000U
#define SIM7670_DATA_ESCAPE_GUARD_MS 1100U
#define SIM7670_POWER_OFF_SETTLE_MS 100U
#define SIM7670_POWER_ON_SETTLE_MS 5000U
#define SIM7670_POWER_CYCLE_OFF_MS 1100U
#define SIM7670_RESET_PULSE_MS 500U
#define SIM7670_RESET_SETTLE_MS 1000U
#define SIM7670_POWER_OFF_TIMEOUT_MS 5000U
#define SIM7670_UART_VERIFY_TIMEOUT_MS 1500U
#define SIM7670_UART_PROBE_ATTEMPTS 3U
#define SIM7670_UART_PROBE_RETRY_MS 100U
#define SIM7670_UART_SWITCH_SETTLE_MS 50U

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char uart_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    bool power_control;
    bool reset_control;
    bool powered;
    bool gnss_powered;
    uint32_t boot_baud_rate;
    uint32_t configured_baud_rate;
    uint32_t active_baud_rate;
    solar_os_gpio_line_ref_t power_line;
    solar_os_gpio_line_ref_t reset_line;
    sim7670_t modem;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    sim7670_status_t cached_status;
    bool cached_status_valid;
#if CONFIG_LWIP_PPP_SUPPORT
    solar_os_modem_profile_t applied_profile;
    bool applied_profile_valid;
    solar_os_ppp_t *ppp;
#endif
} solar_os_sim7670_device_t;

static const char *TAG = "sim7670";
static solar_os_sim7670_device_t devices[SIM7670_DEVICE_MAX];

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *uart_bus,
                                size_t uart_bus_len,
                                bool *power_control,
                                solar_os_gpio_line_ref_t *power_line,
                                bool *reset_control,
                                solar_os_gpio_line_ref_t *reset_line)
{
    bool have_uart = false;
    if (bindings == NULL || uart_bus == NULL || power_control == NULL ||
        power_line == NULL || reset_control == NULL || reset_line == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uart_bus[0] = '\0';
    *power_control = false;
    *reset_control = false;
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_UART_PORT && !have_uart) {
            strlcpy(uart_bus, binding->target, uart_bus_len);
            have_uart = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO_LINE &&
                   strcmp(binding->role, "power") == 0 && !*power_control) {
            strlcpy(power_line->controller,
                    binding->target,
                    sizeof(power_line->controller));
            power_line->line = (uint8_t)binding->value;
            *power_control = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO_LINE &&
                   strcmp(binding->role, "reset") == 0 && !*reset_control) {
            strlcpy(reset_line->controller,
                    binding->target,
                    sizeof(reset_line->controller));
            reset_line->line = (uint8_t)binding->value;
            *reset_control = true;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_uart && solar_os_expansion_find_uart_port(uart_bus, NULL, NULL)
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

static esp_err_t modem_write(void *user,
                             const uint8_t *data,
                             size_t len,
                             size_t *written)
{
    solar_os_sim7670_device_t *device = user;
    if (device == NULL || !device->active || !device->powered) {
        return ESP_ERR_INVALID_STATE;
    }
    return solar_os_bus_uart_write(device->uart_bus, data, len, written);
}

static esp_err_t modem_read(void *user,
                            uint8_t *data,
                            size_t len,
                            uint32_t timeout_ms,
                            size_t *read_len)
{
    solar_os_sim7670_device_t *device = user;
    if (device == NULL || !device->active || !device->powered) {
        return ESP_ERR_INVALID_STATE;
    }
    return solar_os_bus_uart_read(device->uart_bus,
                                  data,
                                  len,
                                  timeout_ms,
                                  read_len);
}

static solar_os_sim7670_device_t *find_device(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SIM7670_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

#if CONFIG_LWIP_PPP_SUPPORT
static void secure_zero(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    while (size-- > 0U) {
        *bytes++ = 0U;
    }
}

static esp_err_t ppp_link_start(void *ctx)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active || !device->powered) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t ret = sim7670_set_packet_attached(&device->modem, true);
    if (ret == ESP_OK) {
        ret = sim7670_read_status(&device->modem, &device->cached_status);
        device->cached_status_valid = ret == ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = sim7670_enter_data_mode(&device->modem);
    }
    if (ret == ESP_OK && device->cached_status_valid) {
        device->cached_status.data_status_valid = true;
        device->cached_status.data_active = true;
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t ppp_link_stop(void *ctx)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(SIM7670_DATA_ESCAPE_GUARD_MS));
    static const uint8_t escape[] = {'+', '+', '+'};
    size_t written = 0U;
    esp_err_t ret = solar_os_bus_uart_write(device->uart_bus,
                                            escape,
                                            sizeof(escape),
                                            &written);
    if (ret == ESP_OK && written != sizeof(escape)) {
        ret = ESP_ERR_INVALID_SIZE;
    }
    vTaskDelay(pdMS_TO_TICKS(SIM7670_DATA_ESCAPE_GUARD_MS));
    char response[128];
    if (ret == ESP_OK) {
        ret = sim7670_command(&device->modem,
                              "AT",
                              3000U,
                              response,
                              sizeof(response));
    }
    if (ret == ESP_OK) {
        ret = sim7670_command(&device->modem,
                              "ATH",
                              50000U,
                              response,
                              sizeof(response));
    }
    if (ret == ESP_OK) {
        sim7670_status_t status;
        if (sim7670_read_status(&device->modem, &status) == ESP_OK) {
            device->cached_status = status;
            device->cached_status_valid = true;
        } else if (device->cached_status_valid) {
            device->cached_status.data_status_valid = true;
            device->cached_status.data_active = false;
        }
    }
    xSemaphoreGive(device->mutex);
    return ret;
}
#endif
static solar_os_modem_network_registration_t map_registration(
    sim7670_registration_t registration)
{
    switch (registration) {
    case SIM7670_REGISTRATION_NOT_REGISTERED:
        return SOLAR_OS_MODEM_REGISTRATION_NOT_REGISTERED;
    case SIM7670_REGISTRATION_SEARCHING:
        return SOLAR_OS_MODEM_REGISTRATION_SEARCHING;
    case SIM7670_REGISTRATION_DENIED:
        return SOLAR_OS_MODEM_REGISTRATION_DENIED;
    case SIM7670_REGISTRATION_HOME:
        return SOLAR_OS_MODEM_REGISTRATION_HOME;
    case SIM7670_REGISTRATION_ROAMING:
        return SOLAR_OS_MODEM_REGISTRATION_ROAMING;
    case SIM7670_REGISTRATION_UNKNOWN:
    default:
        return SOLAR_OS_MODEM_REGISTRATION_UNKNOWN;
    }
}

#if CONFIG_LWIP_PPP_SUPPORT
static solar_os_modem_network_state_t map_ppp_state(
    solar_os_ppp_state_t state)
{
    switch (state) {
    case SOLAR_OS_PPP_STATE_DOWN:
        return SOLAR_OS_MODEM_NETWORK_DOWN;
    case SOLAR_OS_PPP_STATE_CONNECTING:
        return SOLAR_OS_MODEM_NETWORK_CONNECTING;
    case SOLAR_OS_PPP_STATE_UP:
        return SOLAR_OS_MODEM_NETWORK_UP;
    case SOLAR_OS_PPP_STATE_FAILED:
    default:
        return SOLAR_OS_MODEM_NETWORK_FAILED;
    }
}
#endif

static esp_err_t modem_get_status(void *ctx, solar_os_modem_status_t *status)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!device->powered) {
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_LWIP_PPP_SUPPORT
    solar_os_ppp_status_t ppp_status = {0};
    const bool ppp_busy = solar_os_ppp_is_busy(device->ppp);
    esp_err_t ret = solar_os_ppp_get_status(device->ppp, &ppp_status);
    if (ret != ESP_OK) {
        return ret;
    }
#else
    esp_err_t ret = ESP_OK;
#endif
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    sim7670_status_t modem_status = {0};
#if CONFIG_LWIP_PPP_SUPPORT
    if (ppp_busy) {
        if (device->cached_status_valid) {
            modem_status = device->cached_status;
        } else {
            modem_status.online = true;
            modem_status.data_status_valid = true;
            modem_status.data_active = true;
        }
    } else
#endif
    {
        ret = sim7670_read_status(&device->modem, &modem_status);
        if (ret == ESP_OK) {
            device->cached_status = modem_status;
            device->cached_status_valid = true;
        }
    }
#if CONFIG_LWIP_PPP_SUPPORT
    if (ret == ESP_OK) {
        modem_status.data_status_valid = true;
        modem_status.data_active = ppp_busy;
    }
#endif
    if (ret == ESP_OK) {
        *status = (solar_os_modem_status_t) {
            .online = modem_status.online,
            .sim_status_valid = modem_status.sim_status_valid,
            .sim_ready = modem_status.sim_ready,
            .registration_status_valid =
                modem_status.registration_status_valid,
            .registration = map_registration(modem_status.registration),
            .signal_status_valid = modem_status.signal_status_valid,
            .rssi_valid = modem_status.rssi_valid,
            .rssi_dbm = modem_status.rssi_dbm,
            .bit_error_rate = modem_status.bit_error_rate,
            .data_status_valid = modem_status.data_status_valid,
            .data_active = modem_status.data_active,
        };
#if CONFIG_LWIP_PPP_SUPPORT
        status->network_status_valid = true;
        status->network_state = map_ppp_state(ppp_status.state);
        strlcpy(status->network_interface,
                ppp_status.interface_name,
                sizeof(status->network_interface));
        strlcpy(status->ipv4_address,
                ppp_status.ipv4_address,
                sizeof(status->ipv4_address));
        strlcpy(status->ipv4_gateway,
                ppp_status.ipv4_gateway,
                sizeof(status->ipv4_gateway));
        strlcpy(status->dns_address,
                ppp_status.dns_address,
                sizeof(status->dns_address));
#endif
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t modem_apply_profile(
    void *ctx,
    const solar_os_modem_profile_t *profile)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active || profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!device->powered) {
        return ESP_ERR_INVALID_STATE;
    }
    sim7670_pdp_type_t pdp_type;
    switch (profile->ip_type) {
    case SOLAR_OS_MODEM_IP_IPV4:
        pdp_type = SIM7670_PDP_IPV4;
        break;
    case SOLAR_OS_MODEM_IP_IPV6:
        pdp_type = SIM7670_PDP_IPV6;
        break;
    case SOLAR_OS_MODEM_IP_IPV4V6:
        pdp_type = SIM7670_PDP_IPV4V6;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    sim7670_auth_t auth;
    switch (profile->auth) {
    case SOLAR_OS_MODEM_AUTH_NONE:
        auth = SIM7670_AUTH_NONE;
        break;
    case SOLAR_OS_MODEM_AUTH_PAP:
        auth = SIM7670_AUTH_PAP;
        break;
    case SOLAR_OS_MODEM_AUTH_CHAP:
        auth = SIM7670_AUTH_CHAP;
        break;
    case SOLAR_OS_MODEM_AUTH_AUTO:
        auth = SIM7670_AUTH_AUTO;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
#if CONFIG_LWIP_PPP_SUPPORT
    if (solar_os_ppp_is_busy(device->ppp)) {
        ret = ESP_ERR_INVALID_STATE;
    }
#endif
    if (ret == ESP_OK) {
        ret = sim7670_configure_pdp(&device->modem,
                                    profile->apn,
                                    pdp_type,
                                    auth,
                                    profile->username,
                                    profile->password);
    }
#if CONFIG_LWIP_PPP_SUPPORT
    if (ret == ESP_OK) {
        secure_zero(&device->applied_profile,
                    sizeof(device->applied_profile));
        device->applied_profile = *profile;
        device->applied_profile_valid = true;
    }
#endif
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t modem_clear_profile(void *ctx)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!device->powered) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
#if CONFIG_LWIP_PPP_SUPPORT
    if (solar_os_ppp_is_busy(device->ppp)) {
        ret = ESP_ERR_INVALID_STATE;
    }
#endif
    if (ret == ESP_OK) {
        ret = sim7670_clear_pdp(&device->modem);
    }
#if CONFIG_LWIP_PPP_SUPPORT
    if (ret == ESP_OK) {
        secure_zero(&device->applied_profile,
                    sizeof(device->applied_profile));
        device->applied_profile_valid = false;
    }
#endif
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t modem_set_data_active(void *ctx, bool active)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!device->powered) {
        return active ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
#if CONFIG_LWIP_PPP_SUPPORT
    if (!active) {
        const esp_err_t ret = solar_os_ppp_disconnect(device->ppp);
        xSemaphoreTake(device->mutex, portMAX_DELAY);
        secure_zero(&device->applied_profile,
                    sizeof(device->applied_profile));
        device->applied_profile_valid = false;
        xSemaphoreGive(device->mutex);
        return ret;
    }
    solar_os_modem_profile_t modem_profile = {0};
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const bool profile_valid = device->applied_profile_valid;
    if (profile_valid) {
        modem_profile = device->applied_profile;
    }
    xSemaphoreGive(device->mutex);
    if (!profile_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_ppp_profile_t ppp_profile = {0};
    if (modem_profile.auth == SOLAR_OS_MODEM_AUTH_PAP) {
        ppp_profile.auth = SOLAR_OS_PPP_AUTH_PAP;
    } else if (modem_profile.auth == SOLAR_OS_MODEM_AUTH_CHAP) {
        ppp_profile.auth = SOLAR_OS_PPP_AUTH_CHAP;
    } else {
        ppp_profile.auth = SOLAR_OS_PPP_AUTH_NONE;
    }
    strlcpy(ppp_profile.username,
            modem_profile.username,
            sizeof(ppp_profile.username));
    strlcpy(ppp_profile.password,
            modem_profile.password,
            sizeof(ppp_profile.password));
    strlcpy(ppp_profile.dns,
            modem_profile.dns,
            sizeof(ppp_profile.dns));
    const esp_err_t ret = solar_os_ppp_connect(
        device->ppp,
        &ppp_profile,
        SIM7670_PPP_CONNECT_TIMEOUT_MS);
    secure_zero(&ppp_profile, sizeof(ppp_profile));
    secure_zero(&modem_profile, sizeof(modem_profile));
    return ret;
#else
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = sim7670_set_pdp_active(&device->modem, active);
    xSemaphoreGive(device->mutex);
    return ret;
#endif
}

static esp_err_t modem_unlock_sim(void *ctx, const char *pin)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!device->powered) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
#if CONFIG_LWIP_PPP_SUPPORT
    if (solar_os_ppp_is_busy(device->ppp)) {
        ret = ESP_ERR_INVALID_STATE;
    }
#endif
    if (ret == ESP_OK) {
        ret = sim7670_unlock_sim(&device->modem, pin);
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t modem_command(void *ctx,
                               const char *command,
                               uint32_t timeout_ms,
                               char *response,
                               size_t response_size)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!device->powered) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
#if CONFIG_LWIP_PPP_SUPPORT
    if (solar_os_ppp_is_busy(device->ppp)) {
        ret = ESP_ERR_INVALID_STATE;
    }
#endif
    if (ret == ESP_OK) {
        ret = sim7670_command(&device->modem,
                              command,
                              timeout_ms,
                              response,
                              response_size);
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static void clear_power_dependent_state_locked(
    solar_os_sim7670_device_t *device)
{
    device->gnss_powered = false;
    memset(&device->cached_status, 0, sizeof(device->cached_status));
    device->cached_status_valid = false;
#if CONFIG_LWIP_PPP_SUPPORT
    secure_zero(&device->applied_profile,
                sizeof(device->applied_profile));
    device->applied_profile_valid = false;
#endif
}

static esp_err_t set_host_baud_locked(solar_os_sim7670_device_t *device,
                                      uint32_t baud_rate)
{
    const esp_err_t ret = solar_os_bus_uart_set_baud_rate(device->uart_bus,
                                                          baud_rate,
                                                          device->name);
    if (ret == ESP_OK) {
        device->active_baud_rate = baud_rate;
    }
    return ret;
}

static esp_err_t probe_modem_locked(solar_os_sim7670_device_t *device)
{
    esp_err_t ret = ESP_FAIL;
    for (size_t attempt = 0U; attempt < SIM7670_UART_PROBE_ATTEMPTS; attempt++) {
        char response[64];
        ret = sim7670_command(&device->modem,
                              "AT",
                              SIM7670_UART_VERIFY_TIMEOUT_MS,
                              response,
                              sizeof(response));
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        if (attempt + 1U < SIM7670_UART_PROBE_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(SIM7670_UART_PROBE_RETRY_MS));
        }
    }
    return ret;
}

static esp_err_t recover_boot_baud_locked(solar_os_sim7670_device_t *device)
{
    ESP_RETURN_ON_ERROR(set_host_baud_locked(device,
                                             device->boot_baud_rate),
                        TAG,
                        "restore boot baud failed");
    if (probe_modem_locked(device) == ESP_OK) {
        return ESP_OK;
    }

    clear_power_dependent_state_locked(device);
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    if (device->reset_control) {
        ret = solar_os_gpio_line_write(&device->reset_line, false);
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(SIM7670_RESET_PULSE_MS));
            ret = solar_os_gpio_line_write(&device->reset_line, true);
        }
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(SIM7670_RESET_SETTLE_MS));
        }
    } else if (device->power_control) {
        ret = solar_os_gpio_line_write(&device->power_line, false);
        if (ret == ESP_OK) {
            device->powered = false;
            vTaskDelay(pdMS_TO_TICKS(SIM7670_POWER_CYCLE_OFF_MS));
            ret = solar_os_gpio_line_write(&device->power_line, true);
        }
        if (ret == ESP_OK) {
            device->powered = true;
            vTaskDelay(pdMS_TO_TICKS(SIM7670_POWER_ON_SETTLE_MS));
        }
    }
    if (ret != ESP_OK) {
        return ret;
    }
    return probe_modem_locked(device);
}

static esp_err_t prepare_uart_locked(solar_os_sim7670_device_t *device)
{
    const uint32_t target_baud_rate = device->configured_baud_rate != 0U
        ? device->configured_baud_rate
        : SIM7670_UART_MAX_BAUD_RATE;
    ESP_RETURN_ON_ERROR(set_host_baud_locked(device,
                                             device->boot_baud_rate),
                        TAG,
                        "set modem boot baud failed");
    esp_err_t boot_probe = probe_modem_locked(device);
    if (boot_probe != ESP_OK &&
        device->boot_baud_rate != target_baud_rate) {
        const esp_err_t fast_set = set_host_baud_locked(
            device,
            target_baud_rate);
        if (fast_set == ESP_OK && probe_modem_locked(device) == ESP_OK) {
            ESP_LOGI(TAG,
                     "%s transport already at %u baud",
                     device->name,
                     (unsigned)target_baud_rate);
            return ESP_OK;
        }
        (void)set_host_baud_locked(device, device->boot_baud_rate);
    }
    if (boot_probe != ESP_OK) {
        const esp_err_t recovery_ret = recover_boot_baud_locked(device);
        if (recovery_ret == ESP_OK) {
            ESP_LOGW(TAG,
                     "%s transport recovered at %u baud; configured baud deferred",
                     device->name,
                     (unsigned)device->boot_baud_rate);
            return ESP_OK;
        }
        return recovery_ret;
    }
    if (device->boot_baud_rate == target_baud_rate) {
        return ESP_OK;
    }

    const esp_err_t command_ret = sim7670_set_uart_baud_rate(
        &device->modem,
        target_baud_rate);
    if (command_ret == ESP_FAIL) {
        ESP_LOGW(TAG,
                 "%s rejected configured %u baud; continuing at %u baud",
                 device->name,
                 (unsigned)target_baud_rate,
                 (unsigned)device->boot_baud_rate);
        return ESP_OK;
    }
    vTaskDelay(pdMS_TO_TICKS(SIM7670_UART_SWITCH_SETTLE_MS));
    const esp_err_t switch_ret = set_host_baud_locked(
        device,
        target_baud_rate);
    if (switch_ret == ESP_OK && probe_modem_locked(device) == ESP_OK) {
        ESP_LOGI(TAG,
                 "%s transport changed from %u to %u baud%s",
                 device->name,
                 (unsigned)device->boot_baud_rate,
                 (unsigned)target_baud_rate,
                 command_ret == ESP_OK ? "" : " after an unconfirmed response");
        return ESP_OK;
    }

    const esp_err_t recovery_ret = recover_boot_baud_locked(device);
    if (recovery_ret == ESP_OK) {
        ESP_LOGW(TAG,
                 "%s configured baud unavailable; continuing at %u baud",
                 device->name,
                 (unsigned)device->boot_baud_rate);
        return ESP_OK;
    }
    ESP_LOGE(TAG,
             "%s baud negotiation failed: command=%s switch=%s recovery=%s",
             device->name,
             esp_err_to_name(command_ret),
             esp_err_to_name(switch_ret),
             esp_err_to_name(recovery_ret));
    return recovery_ret;
}

static bool transport_rate_supported(uint32_t rate)
{
    switch (rate) {
    case 600U:
    case 1200U:
    case 2400U:
    case 4800U:
    case 9600U:
    case 19200U:
    case 38400U:
    case 57600U:
    case 115200U:
    case 230400U:
    case SIM7670_UART_MAX_BAUD_RATE:
        return true;
    default:
        return false;
    }
}

static esp_err_t modem_get_transport_rate(
    void *ctx,
    solar_os_modem_transport_rate_info_t *info)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    static const uint32_t supported_rates[] = {
        600U, 1200U, 2400U, 4800U, 9600U, 19200U,
        38400U, 57600U, 115200U, 230400U,
        SIM7670_UART_MAX_BAUD_RATE,
    };
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    *info = (solar_os_modem_transport_rate_info_t) {
        .automatic = device->configured_baud_rate == 0U,
        .configured_rate = device->configured_baud_rate,
        .active_rate = device->active_baud_rate,
        .supported_rate_count = sizeof(supported_rates) /
            sizeof(supported_rates[0]),
    };
    memcpy(info->supported_rates,
           supported_rates,
           sizeof(supported_rates));
    xSemaphoreGive(device->mutex);
    return ESP_OK;
}

static esp_err_t modem_set_transport_rate(void *ctx, uint32_t rate)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active ||
        (rate != 0U && !transport_rate_supported(rate))) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t target_rate = rate == 0U
        ? SIM7670_UART_MAX_BAUD_RATE
        : rate;
    xSemaphoreTake(device->mutex, portMAX_DELAY);
#if CONFIG_LWIP_PPP_SUPPORT
    if (solar_os_ppp_is_busy(device->ppp)) {
        xSemaphoreGive(device->mutex);
        return ESP_ERR_INVALID_STATE;
    }
#endif
    if (!device->powered || device->active_baud_rate == target_rate) {
        device->configured_baud_rate = rate;
        xSemaphoreGive(device->mutex);
        return ESP_OK;
    }

    const uint32_t previous_active_rate = device->active_baud_rate;
    const esp_err_t command_ret = sim7670_set_uart_baud_rate(&device->modem,
                                                             target_rate);
    if (command_ret == ESP_FAIL) {
        xSemaphoreGive(device->mutex);
        return command_ret;
    }
    vTaskDelay(pdMS_TO_TICKS(SIM7670_UART_SWITCH_SETTLE_MS));
    esp_err_t ret = set_host_baud_locked(device, target_rate);
    if (ret == ESP_OK) {
        ret = probe_modem_locked(device);
    }
    if (ret == ESP_OK) {
        device->configured_baud_rate = rate;
        ESP_LOGI(TAG,
                 "%s transport changed from %u to %u baud%s",
                 device->name,
                 (unsigned)previous_active_rate,
                 (unsigned)target_rate,
                 command_ret == ESP_OK ? "" : " after an unconfirmed response");
        xSemaphoreGive(device->mutex);
        return ESP_OK;
    }

    const esp_err_t restore_ret = set_host_baud_locked(device,
                                                        previous_active_rate);
    if (restore_ret == ESP_OK && probe_modem_locked(device) == ESP_OK) {
        device->active_baud_rate = previous_active_rate;
    } else {
        (void)recover_boot_baud_locked(device);
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

#if CONFIG_LWIP_PPP_SUPPORT
static void stop_ppp_for_hardware_control(solar_os_sim7670_device_t *device)
{
    if (!solar_os_ppp_is_busy(device->ppp)) {
        return;
    }
    const esp_err_t ret = solar_os_ppp_disconnect(device->ppp);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "%s PPP shutdown failed before hardware control: %s",
                 device->name,
                 esp_err_to_name(ret));
    }
}

static void notify_ppp_transport_reset(solar_os_sim7670_device_t *device)
{
    const esp_err_t ret = solar_os_ppp_notify_transport_reset(device->ppp);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "%s PPP transport reset notification failed: %s",
                 device->name,
                 esp_err_to_name(ret));
    }
}
#endif

static void notify_gnss_power_off(solar_os_sim7670_device_t *device)
{
    const esp_err_t ret = solar_os_gnss_notify_power_state(device->name, false);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG,
                 "%s GNSS power-state notification failed: %s",
                 device->name,
                 esp_err_to_name(ret));
    }
}

static esp_err_t modem_set_power(void *ctx, bool enabled)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active || !device->power_control) {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_LWIP_PPP_SUPPORT
    if (!enabled) {
        stop_ppp_for_hardware_control(device);
    }
#endif

    xSemaphoreTake(device->mutex, portMAX_DELAY);
    if (device->powered == enabled) {
        xSemaphoreGive(device->mutex);
        return ESP_OK;
    }

    if (enabled) {
        const esp_err_t baud_ret = set_host_baud_locked(
            device,
            device->boot_baud_rate);
        if (baud_ret != ESP_OK) {
            xSemaphoreGive(device->mutex);
            return baud_ret;
        }
    }

    if (!enabled) {
#if CONFIG_LWIP_PPP_SUPPORT
        const bool can_use_at = !solar_os_ppp_is_busy(device->ppp);
#else
        const bool can_use_at = true;
#endif
        if (can_use_at) {
            char response[128];
            const esp_err_t shutdown_ret = sim7670_command(
                &device->modem,
                "AT+CPOF",
                SIM7670_POWER_OFF_TIMEOUT_MS,
                response,
                sizeof(response));
            if (shutdown_ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "%s graceful modem shutdown failed: %s",
                         device->name,
                         esp_err_to_name(shutdown_ret));
            }
        }
    } else if (device->reset_control) {
        const esp_err_t reset_ret =
            solar_os_gpio_line_write(&device->reset_line, true);
        if (reset_ret != ESP_OK) {
            xSemaphoreGive(device->mutex);
            return reset_ret;
        }
    }

    const esp_err_t ret = solar_os_gpio_line_write(&device->power_line, enabled);
    if (ret == ESP_OK) {
        device->powered = enabled;
        if (!enabled) {
            clear_power_dependent_state_locked(device);
        }
    }
    xSemaphoreGive(device->mutex);
    if (ret != ESP_OK) {
        return ret;
    }

    if (enabled) {
        vTaskDelay(pdMS_TO_TICKS(SIM7670_POWER_ON_SETTLE_MS));
        xSemaphoreTake(device->mutex, portMAX_DELAY);
        const esp_err_t baud_ret = prepare_uart_locked(device);
        xSemaphoreGive(device->mutex);
        if (baud_ret != ESP_OK) {
            xSemaphoreTake(device->mutex, portMAX_DELAY);
            const esp_err_t power_ret = solar_os_gpio_line_write(
                &device->power_line,
                false);
            if (power_ret == ESP_OK) {
                device->powered = false;
                clear_power_dependent_state_locked(device);
            }
            xSemaphoreGive(device->mutex);
            return baud_ret;
        }
    } else {
        vTaskDelay(pdMS_TO_TICKS(SIM7670_POWER_OFF_SETTLE_MS));
#if CONFIG_LWIP_PPP_SUPPORT
        notify_ppp_transport_reset(device);
#endif
        notify_gnss_power_off(device);
    }
    return ESP_OK;
}

static esp_err_t modem_reset(void *ctx)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active || !device->powered ||
        (!device->reset_control && !device->power_control)) {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_LWIP_PPP_SUPPORT
    stop_ppp_for_hardware_control(device);
#endif

    if (device->reset_control) {
        xSemaphoreTake(device->mutex, portMAX_DELAY);
        esp_err_t ret = set_host_baud_locked(device,
                                             device->boot_baud_rate);
        if (ret == ESP_OK) {
            ret = solar_os_gpio_line_write(&device->reset_line, false);
        }
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(SIM7670_RESET_PULSE_MS));
            ret = solar_os_gpio_line_write(&device->reset_line, true);
        }
        if (ret == ESP_OK) {
            clear_power_dependent_state_locked(device);
        }
        xSemaphoreGive(device->mutex);
        if (ret != ESP_OK) {
            return ret;
        }
#if CONFIG_LWIP_PPP_SUPPORT
        notify_ppp_transport_reset(device);
#endif
        notify_gnss_power_off(device);
        vTaskDelay(pdMS_TO_TICKS(SIM7670_RESET_SETTLE_MS));
        xSemaphoreTake(device->mutex, portMAX_DELAY);
        ret = prepare_uart_locked(device);
        xSemaphoreGive(device->mutex);
        return ret;
    }

    ESP_RETURN_ON_ERROR(modem_set_power(device, false),
                        TAG,
                        "modem power off failed");
    vTaskDelay(pdMS_TO_TICKS(SIM7670_POWER_CYCLE_OFF_MS));
    ESP_RETURN_ON_ERROR(modem_set_power(device, true),
                        TAG,
                        "modem power on failed");
    vTaskDelay(pdMS_TO_TICKS(SIM7670_RESET_SETTLE_MS));
    return ESP_OK;
}

static const solar_os_modem_ops_t modem_ops = {
    .get_status = modem_get_status,
    .apply_profile = modem_apply_profile,
    .clear_profile = modem_clear_profile,
    .set_data_active = modem_set_data_active,
    .get_transport_rate = modem_get_transport_rate,
    .set_transport_rate = modem_set_transport_rate,
    .unlock_sim = modem_unlock_sim,
    .command = modem_command,
};

static const solar_os_modem_ops_t powered_modem_ops = {
    .get_status = modem_get_status,
    .set_power = modem_set_power,
    .reset = modem_reset,
    .apply_profile = modem_apply_profile,
    .clear_profile = modem_clear_profile,
    .set_data_active = modem_set_data_active,
    .get_transport_rate = modem_get_transport_rate,
    .set_transport_rate = modem_set_transport_rate,
    .unlock_sim = modem_unlock_sim,
    .command = modem_command,
};

static const solar_os_modem_ops_t resettable_modem_ops = {
    .get_status = modem_get_status,
    .reset = modem_reset,
    .apply_profile = modem_apply_profile,
    .clear_profile = modem_clear_profile,
    .set_data_active = modem_set_data_active,
    .get_transport_rate = modem_get_transport_rate,
    .set_transport_rate = modem_set_transport_rate,
    .unlock_sim = modem_unlock_sim,
    .command = modem_command,
};

static esp_err_t gnss_read_fix(void *ctx,
                               uint32_t timeout_ms,
                               solar_os_gnss_fix_t *fix)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active || fix == NULL || timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!device->powered) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
#if CONFIG_LWIP_PPP_SUPPORT
    if (solar_os_ppp_is_busy(device->ppp)) {
        xSemaphoreGive(device->mutex);
        return ESP_ERR_INVALID_STATE;
    }
#endif
    if (!device->gnss_powered) {
        xSemaphoreGive(device->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    sim7670_gnss_fix_t modem_fix;
    const esp_err_t ret = sim7670_read_gnss_fix(&device->modem,
                                                timeout_ms,
                                                &modem_fix);
    if (ret == ESP_OK) {
        *fix = (solar_os_gnss_fix_t) {
            .valid = modem_fix.valid,
            .time_valid = modem_fix.time_valid,
            .year = modem_fix.year,
            .month = modem_fix.month,
            .day = modem_fix.day,
            .hour = modem_fix.hour,
            .minute = modem_fix.minute,
            .second = modem_fix.second,
            .fix_type = modem_fix.fix_type,
            .satellites_valid = modem_fix.satellites_valid,
            .satellites = modem_fix.satellites,
            .latitude_deg_e7 = modem_fix.latitude_deg_e7,
            .longitude_deg_e7 = modem_fix.longitude_deg_e7,
            .height_msl_mm = modem_fix.height_msl_mm,
            .ground_speed_mm_s = modem_fix.ground_speed_mm_s,
            .heading_deg_e5 = modem_fix.heading_deg_e5,
            .position_dop_e2 = modem_fix.position_dop_e2,
        };
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t gnss_set_power(void *ctx, bool enabled)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!enabled && device->power_control) {
        return solar_os_modem_set_power(device->name, false);
    }
    if (enabled && device->power_control && !device->powered) {
        const esp_err_t power_ret =
            solar_os_modem_set_power(device->name, true);
        if (power_ret != ESP_OK) {
            return power_ret;
        }
    }
    if (!device->powered) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
#if CONFIG_LWIP_PPP_SUPPORT
    if (solar_os_ppp_is_busy(device->ppp)) {
        xSemaphoreGive(device->mutex);
        return ESP_ERR_INVALID_STATE;
    }
#endif
    esp_err_t ret = ESP_OK;
    if (device->gnss_powered != enabled) {
        ret = sim7670_set_gnss_power(&device->modem, enabled);
    }
    if (ret == ESP_OK) {
        device->gnss_powered = enabled;
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static const solar_os_gnss_ops_t gnss_ops = {
    .read_fix = gnss_read_fix,
    .set_power = gnss_set_power,
};

esp_err_t solar_os_sim7670_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_sim7670_device_t *device = NULL;
    for (size_t i = 0; i < SIM7670_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!devices[i].active && device == NULL) {
            device = &devices[i];
        }
    }
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char uart_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    bool power_control = false;
    bool reset_control = false;
    solar_os_gpio_line_ref_t power_line = {0};
    solar_os_gpio_line_ref_t reset_line = {0};
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       uart_bus,
                                       sizeof(uart_bus),
                                       &power_control,
                                       &power_line,
                                       &reset_control,
                                       &reset_line),
                        TAG,
                        "invalid bindings");
    solar_os_bus_info_t uart_info;
    if (!solar_os_bus_find(uart_bus,
                           SOLAR_OS_BUS_PROTOCOL_UART,
                           &uart_info) ||
        uart_info.config.uart.baud_rate > SIM7670_UART_MAX_BAUD_RATE) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(device, 0, sizeof(*device));
    device->active = true;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->uart_bus, uart_bus, sizeof(device->uart_bus));
    device->power_control = power_control;
    device->reset_control = reset_control;
    device->power_line = power_line;
    device->reset_line = reset_line;
    device->powered = !power_control;
    device->boot_baud_rate = uart_info.config.uart.baud_rate;
    device->active_baud_rate = device->boot_baud_rate;
    device->mutex = xSemaphoreCreateMutexStatic(&device->mutex_storage);
    if (device->mutex == NULL) {
        memset(device, 0, sizeof(*device));
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = ESP_OK;
    if (reset_control) {
        ret = solar_os_gpio_line_write(&device->reset_line, true);
    }
    if (ret == ESP_OK && power_control) {
        ret = solar_os_gpio_line_write(&device->power_line, true);
        device->powered = ret == ESP_OK;
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(SIM7670_POWER_ON_SETTLE_MS));
        }
    }
    if (ret != ESP_OK) {
        if (power_control) {
            (void)solar_os_gpio_line_write(&device->power_line, false);
        }
        memset(device, 0, sizeof(*device));
        return ret;
    }
    const sim7670_io_t io = {
        .write = modem_write,
        .read = modem_read,
        .user = device,
    };
    ret = sim7670_init(&device->modem, &io);
    if (ret != ESP_OK) {
        if (power_control) {
            (void)solar_os_gpio_line_write(&device->power_line, false);
        }
        memset(device, 0, sizeof(*device));
        return ret;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    ret = prepare_uart_locked(device);
    xSemaphoreGive(device->mutex);
    if (ret != ESP_OK) {
        if (power_control) {
            (void)solar_os_gpio_line_write(&device->power_line, false);
        }
        memset(device, 0, sizeof(*device));
        return ret;
    }
#if CONFIG_LWIP_PPP_SUPPORT
    const solar_os_ppp_config_t ppp_config = {
        .name = name,
        .route_priority = 110,
        .read_timeout_ms = SIM7670_PPP_READ_TIMEOUT_MS,
        .transport = {
            .start = ppp_link_start,
            .stop = ppp_link_stop,
            .read = modem_read,
            .write = modem_write,
            .ctx = device,
        },
    };
    ret = solar_os_ppp_create(&ppp_config, &device->ppp);
    if (ret != ESP_OK) {
        if (power_control) {
            (void)solar_os_gpio_line_write(&device->power_line, false);
        }
        memset(device, 0, sizeof(*device));
        return ret;
    }
#endif

    const solar_os_modem_registration_t modem_registration = {
        .name = name,
        .driver = "sim7670",
        .transport = device->uart_bus,
        .ops = power_control
            ? &powered_modem_ops
            : (reset_control ? &resettable_modem_ops : &modem_ops),
        .ctx = device,
        .powered = device->powered,
    };
    ret = solar_os_modem_register(&modem_registration);
    if (ret != ESP_OK) {
#if CONFIG_LWIP_PPP_SUPPORT
        (void)solar_os_ppp_destroy(device->ppp);
#endif
        if (power_control) {
            (void)solar_os_gpio_line_write(&device->power_line, false);
        }
        memset(device, 0, sizeof(*device));
        return ret;
    }

    const solar_os_gnss_registration_t registration = {
        .name = name,
        .driver = "sim7670",
        .ops = &gnss_ops,
        .ctx = device,
        .powered = false,
    };
    ret = solar_os_gnss_register(&registration);
    if (ret != ESP_OK) {
        (void)solar_os_modem_unregister(name);
#if CONFIG_LWIP_PPP_SUPPORT
        (void)solar_os_ppp_destroy(device->ppp);
#endif
        if (power_control) {
            (void)solar_os_gpio_line_write(&device->power_line, false);
        }
        memset(device, 0, sizeof(*device));
        return ret;
    }
    ESP_LOGI(TAG,
             "%s attached on %s at %u baud power=%s reset=%s",
             name,
             uart_bus,
             (unsigned)device->active_baud_rate,
             power_control ? "controlled" : "always-on",
             reset_control ? "controlled" :
                 (power_control ? "power-cycle" : "none"));
    return ESP_OK;
}

esp_err_t solar_os_sim7670_detach(const char *name)
{
    solar_os_sim7670_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
#if CONFIG_LWIP_PPP_SUPPORT
    if (solar_os_ppp_is_busy(device->ppp)) {
        ESP_RETURN_ON_ERROR(solar_os_ppp_disconnect(device->ppp),
                            TAG,
                            "PPP stop failed");
    }
#endif
    if (device->power_control && device->powered) {
        ESP_RETURN_ON_ERROR(modem_set_power(device, false),
                            TAG,
                            "modem power off failed");
    }
    ESP_RETURN_ON_ERROR(solar_os_gnss_unregister(name),
                        TAG,
                        "GNSS unregister failed");
    ESP_RETURN_ON_ERROR(solar_os_modem_unregister(name),
                        TAG,
                        "modem unregister failed");
#if CONFIG_LWIP_PPP_SUPPORT
    ESP_RETURN_ON_ERROR(solar_os_ppp_destroy(device->ppp),
                        TAG,
                        "PPP destroy failed");
    device->ppp = NULL;
    secure_zero(&device->applied_profile,
                sizeof(device->applied_profile));
#endif
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    device->active = false;
    xSemaphoreGive(device->mutex);
    memset(device, 0, sizeof(*device));
    return ESP_OK;
}

size_t solar_os_sim7670_count(void)
{
    size_t count = 0U;
    for (size_t i = 0; i < SIM7670_DEVICE_MAX; i++) {
        count += devices[i].active ? 1U : 0U;
    }
    return count;
}

bool solar_os_sim7670_get(size_t index, solar_os_sim7670_info_t *info)
{
    if (info == NULL) {
        return false;
    }
    size_t current = 0U;
    for (size_t i = 0; i < SIM7670_DEVICE_MAX; i++) {
        if (!devices[i].active) {
            continue;
        }
        if (current++ == index) {
            xSemaphoreTake(devices[i].mutex, portMAX_DELAY);
            *info = (solar_os_sim7670_info_t) {
                .power_control = devices[i].power_control,
                .reset_control = devices[i].reset_control,
                .powered = devices[i].powered,
                .gnss_powered = devices[i].gnss_powered,
            };
            strlcpy(info->name, devices[i].name, sizeof(info->name));
            strlcpy(info->uart_bus,
                    devices[i].uart_bus,
                    sizeof(info->uart_bus));
            xSemaphoreGive(devices[i].mutex);
            return true;
        }
    }
    return false;
}

esp_err_t solar_os_sim7670_read_status(const char *name,
                                       sim7670_status_t *status)
{
    solar_os_sim7670_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t ret = device->powered ? ESP_OK : ESP_ERR_INVALID_STATE;
#if CONFIG_LWIP_PPP_SUPPORT
    if (solar_os_ppp_is_busy(device->ppp)) {
        ret = ESP_ERR_INVALID_STATE;
    }
#endif
    if (ret == ESP_OK) {
        ret = sim7670_read_status(&device->modem, status);
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

esp_err_t solar_os_sim7670_command(const char *name,
                                   const char *command,
                                   uint32_t timeout_ms,
                                   char *response,
                                   size_t response_size)
{
    solar_os_sim7670_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t ret = device->powered ? ESP_OK : ESP_ERR_INVALID_STATE;
#if CONFIG_LWIP_PPP_SUPPORT
    if (solar_os_ppp_is_busy(device->ppp)) {
        ret = ESP_ERR_INVALID_STATE;
    }
#endif
    if (ret == ESP_OK) {
        ret = sim7670_command(&device->modem,
                              command,
                              timeout_ms,
                              response,
                              response_size);
    }
    xSemaphoreGive(device->mutex);
    return ret;
}
