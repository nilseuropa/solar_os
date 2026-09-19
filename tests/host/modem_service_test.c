#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "solar_os_modem.h"

typedef struct {
    solar_os_modem_profile_t profile;
    solar_os_modem_status_t status;
    unsigned apply_count;
    unsigned clear_count;
    unsigned data_count;
    unsigned unlock_count;
    unsigned power_count;
    unsigned reset_count;
    unsigned status_count;
    bool data_active;
    bool powered;
    bool try_unregister;
    char pin[9];
} fake_modem_t;

static bool nvs_namespace_exists;
static bool nvs_blob_exists;
static char nvs_key[16];
static uint8_t nvs_blob[512];
static size_t nvs_blob_size;

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t length = strlen(src);
    if (size > 0U) {
        const size_t copy = length < size - 1U ? length : size - 1U;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return length;
}

esp_err_t nvs_open(const char *name,
                   nvs_open_mode_t mode,
                   nvs_handle_t *handle)
{
    assert(strcmp(name, "cellular") == 0);
    if (mode == NVS_READONLY && !nvs_namespace_exists) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    nvs_namespace_exists = true;
    *handle = 1U;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle,
                       const char *key,
                       void *value,
                       size_t *length)
{
    assert(handle == 1U);
    if (!nvs_blob_exists || strcmp(key, nvs_key) != 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (*length < nvs_blob_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(value, nvs_blob, nvs_blob_size);
    *length = nvs_blob_size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle,
                       const char *key,
                       const void *value,
                       size_t length)
{
    assert(handle == 1U);
    assert(length <= sizeof(nvs_blob));
    strlcpy(nvs_key, key, sizeof(nvs_key));
    memcpy(nvs_blob, value, length);
    nvs_blob_size = length;
    nvs_blob_exists = true;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    assert(handle == 1U);
    if (!nvs_blob_exists || strcmp(key, nvs_key) != 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    memset(nvs_blob, 0, sizeof(nvs_blob));
    nvs_blob_size = 0U;
    nvs_blob_exists = false;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1U);
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1U);
}

static esp_err_t fake_get_status(void *ctx, solar_os_modem_status_t *status)
{
    fake_modem_t *fake = ctx;
    fake->status_count++;
    if (fake->try_unregister) {
        assert(solar_os_modem_unregister("modem0") == ESP_ERR_INVALID_STATE);
    }
    *status = fake->status;
    status->data_status_valid = true;
    status->data_active = fake->data_active;
    return ESP_OK;
}

static esp_err_t fake_set_power(void *ctx, bool enabled)
{
    fake_modem_t *fake = ctx;
    fake->powered = enabled;
    fake->power_count++;
    return ESP_OK;
}

static esp_err_t fake_reset(void *ctx)
{
    fake_modem_t *fake = ctx;
    assert(fake->powered);
    fake->reset_count++;
    return ESP_OK;
}

static esp_err_t fake_apply_profile(
    void *ctx,
    const solar_os_modem_profile_t *profile)
{
    fake_modem_t *fake = ctx;
    fake->profile = *profile;
    fake->apply_count++;
    return ESP_OK;
}

static esp_err_t fake_clear_profile(void *ctx)
{
    fake_modem_t *fake = ctx;
    memset(&fake->profile, 0, sizeof(fake->profile));
    fake->clear_count++;
    fake->data_active = false;
    return ESP_OK;
}

static esp_err_t fake_set_data_active(void *ctx, bool active)
{
    fake_modem_t *fake = ctx;
    fake->data_active = active;
    fake->data_count++;
    return ESP_OK;
}

static esp_err_t fake_unlock_sim(void *ctx, const char *pin)
{
    fake_modem_t *fake = ctx;
    strlcpy(fake->pin, pin, sizeof(fake->pin));
    fake->unlock_count++;
    return ESP_OK;
}

static esp_err_t fake_command(void *ctx,
                              const char *command,
                              uint32_t timeout_ms,
                              char *response,
                              size_t response_size)
{
    (void)ctx;
    assert(strcmp(command, "AT") == 0);
    assert(timeout_ms == 1000U);
    assert(response_size >= 5U);
    strlcpy(response, "OK\r\n", response_size);
    return ESP_OK;
}

static const solar_os_modem_ops_t fake_ops = {
    .get_status = fake_get_status,
    .set_power = fake_set_power,
    .reset = fake_reset,
    .apply_profile = fake_apply_profile,
    .clear_profile = fake_clear_profile,
    .set_data_active = fake_set_data_active,
    .unlock_sim = fake_unlock_sim,
    .command = fake_command,
};

static const solar_os_modem_ops_t always_on_ops = {
    .get_status = fake_get_status,
};

int main(void)
{
    solar_os_modem_profile_t profile = {
        .apn = "5g.vodafone.iot",
        .ip_type = SOLAR_OS_MODEM_IP_IPV4V6,
        .auth = SOLAR_OS_MODEM_AUTH_NONE,
    };
    assert(solar_os_modem_profile_validate(&profile) == ESP_OK);
    strlcpy(profile.apn, "bad\"apn", sizeof(profile.apn));
    assert(solar_os_modem_profile_validate(&profile) == ESP_ERR_INVALID_ARG);
    strlcpy(profile.apn, "5g.vodafone.iot", sizeof(profile.apn));
    strlcpy(profile.dns, "8.8.8.8", sizeof(profile.dns));
    assert(solar_os_modem_profile_validate(&profile) == ESP_OK);
    strlcpy(profile.dns, "8.8.8.999", sizeof(profile.dns));
    assert(solar_os_modem_profile_validate(&profile) == ESP_ERR_INVALID_ARG);
    strlcpy(profile.dns, "8.8.8.8", sizeof(profile.dns));
    profile.auth = SOLAR_OS_MODEM_AUTH_PAP;
    assert(solar_os_modem_profile_validate(&profile) == ESP_ERR_INVALID_ARG);
    strlcpy(profile.username, "user", sizeof(profile.username));
    strlcpy(profile.password, "pass", sizeof(profile.password));
    assert(solar_os_modem_profile_validate(&profile) == ESP_OK);
    profile.auth = SOLAR_OS_MODEM_AUTH_NONE;
    profile.username[0] = '\0';
    profile.password[0] = '\0';

    fake_modem_t fake = {
        .status = {
            .online = true,
            .sim_status_valid = true,
            .sim_ready = true,
            .registration_status_valid = true,
            .registration = SOLAR_OS_MODEM_REGISTRATION_ROAMING,
        },
        .powered = true,
        .try_unregister = true,
    };
    const solar_os_modem_registration_t registration = {
        .name = "modem0",
        .driver = "fake",
        .transport = "uart0",
        .ops = &fake_ops,
        .ctx = &fake,
        .powered = true,
    };
    assert(solar_os_modem_register(&registration) == ESP_OK);
    assert(solar_os_modem_count() == 1U);
    solar_os_modem_info_t info;
    assert(solar_os_modem_get(0U, &info));
    assert(strcmp(info.name, "modem0") == 0);
    assert(strcmp(info.driver, "fake") == 0);
    assert(info.power_control && info.powered && info.reset_control);
    assert(info.profile_support && info.data_control && info.sim_unlock);
    assert(info.raw_command);

    solar_os_modem_status_t status;
    assert(solar_os_modem_get_status("modem0", &status) == ESP_OK);
    assert(status.power_control && status.powered);
    assert(status.sim_ready);
    assert(status.registration == SOLAR_OS_MODEM_REGISTRATION_ROAMING);

    assert(solar_os_modem_profile_get("modem0", &profile) ==
           ESP_ERR_NOT_FOUND);
    assert(solar_os_modem_profile_set("modem0", &profile) == ESP_OK);
    assert(fake.apply_count == 1U);
    solar_os_modem_profile_t loaded;
    assert(solar_os_modem_profile_get("modem0", &loaded) == ESP_OK);
    assert(strcmp(loaded.apn, profile.apn) == 0);
    assert(strcmp(loaded.dns, profile.dns) == 0);
    assert(loaded.ip_type == SOLAR_OS_MODEM_IP_IPV4V6);

    assert(solar_os_modem_set_data_active("modem0", true) == ESP_OK);
    assert(fake.apply_count == 2U);
    assert(fake.data_count == 1U && fake.data_active);
    assert(solar_os_modem_set_data_active("modem0", false) == ESP_OK);
    assert(fake.data_count == 2U && !fake.data_active);

    assert(solar_os_modem_unlock_sim("modem0", "1234") == ESP_OK);
    assert(fake.unlock_count == 1U && strcmp(fake.pin, "1234") == 0);
    char response[8];
    assert(solar_os_modem_command("modem0",
                                  "AT",
                                  1000U,
                                  response,
                                  sizeof(response)) == ESP_OK);
    assert(strcmp(response, "OK\r\n") == 0);

    assert(solar_os_modem_reset("modem0") == ESP_OK);
    assert(fake.reset_count == 1U);
    assert(solar_os_modem_set_power("modem0", false) == ESP_OK);
    assert(fake.power_count == 1U && !fake.powered);
    assert(solar_os_modem_get(0U, &info));
    assert(info.power_control && !info.powered);
    const unsigned status_count = fake.status_count;
    assert(solar_os_modem_get_status("modem0", &status) == ESP_OK);
    assert(status.power_control && !status.powered && !status.online);
    assert(status.network_status_valid);
    assert(status.network_state == SOLAR_OS_MODEM_NETWORK_DOWN);
    assert(fake.status_count == status_count);
    assert(solar_os_modem_reset("modem0") == ESP_ERR_INVALID_STATE);
    assert(solar_os_modem_command("modem0",
                                  "AT",
                                  1000U,
                                  response,
                                  sizeof(response)) == ESP_ERR_INVALID_STATE);
    assert(solar_os_modem_set_data_active("modem0", false) == ESP_OK);
    assert(fake.data_count == 2U);
    assert(solar_os_modem_set_power("modem0", true) == ESP_OK);
    assert(fake.power_count == 2U && fake.powered);
    assert(solar_os_modem_reset("modem0") == ESP_OK);
    assert(fake.reset_count == 2U);

    assert(solar_os_modem_profile_clear("modem0") == ESP_OK);
    assert(fake.clear_count == 1U);
    assert(solar_os_modem_profile_get("modem0", &loaded) == ESP_ERR_NOT_FOUND);
    assert(solar_os_modem_unregister("modem0") == ESP_OK);
    assert(solar_os_modem_count() == 0U);

    fake_modem_t always_on = {
        .status = {
            .online = true,
        },
        .powered = true,
    };
    const solar_os_modem_registration_t always_on_registration = {
        .name = "modem1",
        .driver = "always-on",
        .transport = "uart1",
        .ops = &always_on_ops,
        .ctx = &always_on,
    };
    assert(solar_os_modem_register(&always_on_registration) == ESP_OK);
    assert(solar_os_modem_get(0U, &info));
    assert(!info.power_control && info.powered && !info.reset_control);
    assert(solar_os_modem_get_status("modem1", &status) == ESP_OK);
    assert(!status.power_control && status.powered && status.online);
    assert(solar_os_modem_set_power("modem1", false) ==
           ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_modem_reset("modem1") == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_modem_unregister("modem1") == ESP_OK);

    solar_os_modem_ip_type_t ip_type;
    solar_os_modem_auth_t auth;
    assert(solar_os_modem_ip_type_parse("ipv6", &ip_type));
    assert(ip_type == SOLAR_OS_MODEM_IP_IPV6);
    assert(solar_os_modem_auth_parse("chap", &auth));
    assert(auth == SOLAR_OS_MODEM_AUTH_CHAP);
    assert(strcmp(solar_os_modem_registration_name(
                      SOLAR_OS_MODEM_REGISTRATION_HOME),
                  "home") == 0);
    assert(strcmp(solar_os_modem_network_state_name(
                      SOLAR_OS_MODEM_NETWORK_CONNECTING),
                  "connecting") == 0);

    puts("Modem service registry/profile tests: ok");
    return 0;
}
