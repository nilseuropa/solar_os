#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"
#include "solar_os_modem.h"
#include "solar_os_native_device.h"
#include "solar_os_native_modem_provider.h"
#include "solar_os_native_runtime.h"
#include "solar_os_memory.h"

typedef struct {
    solar_os_modem_profile_t profile;
    solar_os_modem_status_t status;
    unsigned apply_count;
    unsigned clear_count;
    unsigned data_count;
    unsigned unlock_count;
    unsigned power_count;
    unsigned reset_count;
    unsigned transport_rate_set_count;
    unsigned status_count;
    uint32_t configured_transport_rate;
    uint32_t active_transport_rate;
    bool data_active;
    bool powered;
    bool try_unregister;
    char pin[9];
} fake_modem_t;

static bool nvs_namespace_exists;

typedef struct {
    bool exists;
    char key[16];
    uint8_t blob[512];
    size_t size;
} fake_nvs_entry_t;

static fake_nvs_entry_t nvs_entries[4];

static fake_nvs_entry_t *find_nvs_entry(const char *key, bool create)
{
    fake_nvs_entry_t *free_entry = NULL;
    for (size_t i = 0U; i < sizeof(nvs_entries) / sizeof(nvs_entries[0]); i++) {
        if (nvs_entries[i].exists && strcmp(nvs_entries[i].key, key) == 0) {
            return &nvs_entries[i];
        }
        if (!nvs_entries[i].exists && free_entry == NULL) {
            free_entry = &nvs_entries[i];
        }
    }
    if (create && free_entry != NULL) {
        free_entry->exists = true;
        (void)snprintf(free_entry->key, sizeof(free_entry->key), "%s", key);
        return free_entry;
    }
    return NULL;
}

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
    fake_nvs_entry_t *entry = find_nvs_entry(key, false);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (*length < entry->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(value, entry->blob, entry->size);
    *length = entry->size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle,
                       const char *key,
                       const void *value,
                       size_t length)
{
    assert(handle == 1U);
    fake_nvs_entry_t *entry = find_nvs_entry(key, true);
    assert(entry != NULL);
    assert(length <= sizeof(entry->blob));
    memcpy(entry->blob, value, length);
    entry->size = length;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    assert(handle == 1U);
    fake_nvs_entry_t *entry = find_nvs_entry(key, false);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    memset(entry, 0, sizeof(*entry));
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

static esp_err_t fake_get_transport_rate(
    void *ctx,
    solar_os_modem_transport_rate_info_t *info)
{
    fake_modem_t *fake = ctx;
    *info = (solar_os_modem_transport_rate_info_t) {
        .automatic = fake->configured_transport_rate == 0U,
        .configured_rate = fake->configured_transport_rate,
        .active_rate = fake->active_transport_rate,
        .supported_rates = {115200U, 230400U, 460800U},
        .supported_rate_count = 3U,
    };
    return ESP_OK;
}

static esp_err_t fake_set_transport_rate(void *ctx, uint32_t rate)
{
    fake_modem_t *fake = ctx;
    if (rate != 0U && rate != 115200U && rate != 230400U &&
        rate != 460800U) {
        return ESP_ERR_INVALID_ARG;
    }
    fake->configured_transport_rate = rate;
    fake->active_transport_rate = rate == 0U ? 460800U : rate;
    fake->transport_rate_set_count++;
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
    .get_transport_rate = fake_get_transport_rate,
    .set_transport_rate = fake_set_transport_rate,
    .unlock_sim = fake_unlock_sim,
    .command = fake_command,
};

static const solar_os_modem_ops_t always_on_ops = {
    .get_status = fake_get_status,
};

static const solar_os_native_modem_provider_api_v1_t *provider;
static solar_os_native_handle_t native_modem;
static unsigned library_pins, restore_calls, allocations;

void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t kind, const char *tag)
{
    assert(kind == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag);
    void *pointer = calloc(count, size);
    if (pointer) ++allocations;
    return pointer;
}

void solar_os_memory_free(void *pointer)
{
    if (pointer) {
        assert(allocations);
        --allocations;
        free(pointer);
    }
}

int solar_os_native_library_retain(solar_os_native_handle_t lease, solar_os_native_handle_t *out)
{
    assert(lease == 123);
    ++library_pins;
    *out = 456;
    return SOLAR_OS_NATIVE_OK;
}

int solar_os_native_library_release(solar_os_native_handle_t lease)
{
    assert(lease == 456 && library_pins);
    --library_pins;
    return SOLAR_OS_NATIVE_OK;
}

static int native_status(void *user, solar_os_native_modem_status_v1_t *status)
{
    assert(library_pins == 1 && native_modem);
    assert(provider->close(native_modem) == SOLAR_OS_NATIVE_ERROR_BUSY);
    return fake_get_status(user, status);
}

static int native_restore_rate(void *user, uint32_t rate)
{
    ++restore_calls;
    assert(library_pins == 1 && solar_os_modem_unregister("modem0") == ESP_ERR_INVALID_STATE);
    return fake_set_transport_rate(user, rate);
}

static void test_native_provider(const solar_os_native_modem_api_v1_t *modem, fake_modem_t *fake)
{
    provider = solar_os_native_modem_provider_get_service(SOLAR_OS_NATIVE_MODEM_PROVIDER_SERVICE, 1, sizeof(*provider));
    assert(provider);
    solar_os_native_modem_registration_v1_t in = {
        .name = "modem0", .driver = "native", .transport = "uart0", .user = fake,
        .operations = {.status = native_status, .get_transport_rate = fake_get_transport_rate,
                       .set_transport_rate = native_restore_rate},
    };
    assert(provider->register_modem(&in, &native_modem) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!native_modem && !allocations && !library_pins);
    in.library_lease = 123;
    fake->try_unregister = false;
    assert(!provider->register_modem(&in, &native_modem));
    assert(restore_calls == 1 && library_pins == 1 && native_modem && fake->configured_transport_rate == 230400U);
    assert(solar_os_modem_unregister_owned(in.name, fake) == ESP_ERR_NOT_FOUND);
    solar_os_native_modem_status_v1_t status;
    assert(!modem->status(in.name, &status));
    assert(modem->count() == 1 && !provider->close(native_modem));
    assert(!modem->count() && !library_pins && !allocations);
    assert(provider->close(native_modem) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
}

int main(void)
{
    _Static_assert(sizeof(solar_os_modem_profile_t) == 260, "retain saved profile layout");
    const solar_os_native_modem_api_v1_t *api = solar_os_native_device_get_service(
        SOLAR_OS_NATIVE_MODEM_SERVICE, SOLAR_OS_NATIVE_MODEM_ABI, sizeof(*api));
    assert(api && !api->count() && !api->label(UINT32_MAX, 0));
    uint32_t parsed;
    assert(!api->parse(SOLAR_OS_MODEM_LABEL_AUTH, "pap", &parsed) && parsed == SOLAR_OS_MODEM_AUTH_PAP);
    assert(api->parse(SOLAR_OS_MODEM_LABEL_AUTH, "invalid", &parsed) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!strcmp(api->label(SOLAR_OS_MODEM_LABEL_IP_TYPE, SOLAR_OS_MODEM_IP_IPV6), "ipv6"));
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
        .active_transport_rate = 460800U,
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
    assert(solar_os_modem_count() == 1U && api->count() == 1U);
    solar_os_modem_info_t info;
    assert(!api->info(0, &info));
    assert(api->info(1, &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->set_active("modem0", UINT32_MAX, true) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(strcmp(info.name, "modem0") == 0);
    assert(strcmp(info.driver, "fake") == 0);
    assert(info.power_control && info.powered && info.reset_control);
    assert(info.profile_support && info.data_control && info.sim_unlock);
    assert(info.transport_rate_control);
    assert(info.raw_command);

    solar_os_modem_transport_rate_info_t rate_info;
    assert(solar_os_modem_transport_rate_get("modem0", &rate_info) == ESP_OK);
    assert(rate_info.automatic && rate_info.configured_rate == 0U);
    assert(rate_info.active_rate == 460800U);
    assert(rate_info.supported_rate_count == 3U);
    assert(solar_os_modem_transport_rate_set("modem0", 230400U) == ESP_OK);
    assert(fake.transport_rate_set_count == 1U);
    assert(solar_os_modem_transport_rate_get("modem0", &rate_info) == ESP_OK);
    assert(!rate_info.automatic && rate_info.configured_rate == 230400U);
    assert(rate_info.active_rate == 230400U);
    assert(solar_os_modem_transport_rate_set("modem0", 921600U) ==
           ESP_ERR_INVALID_ARG);

    solar_os_modem_status_t status;
    assert(solar_os_modem_get_status("modem0", &status) == ESP_OK);
    assert(status.power_control && status.powered);
    assert(status.sim_ready);
    assert(status.registration == SOLAR_OS_MODEM_REGISTRATION_ROAMING);

    assert(solar_os_modem_profile_get("modem0", &profile) ==
           ESP_ERR_NOT_FOUND);
    assert(!api->profile_write("modem0", &profile));
    assert(fake.apply_count == 1U);
    solar_os_modem_profile_t loaded;
    assert(!api->profile_read("modem0", &loaded));
    assert(strcmp(loaded.apn, profile.apn) == 0);
    assert(strcmp(loaded.dns, profile.dns) == 0);
    assert(loaded.ip_type == SOLAR_OS_MODEM_IP_IPV4V6);

    assert(!api->set_active("modem0", SOLAR_OS_MODEM_PACKET_DATA, true));
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
    assert(!api->set_active("modem0", SOLAR_OS_MODEM_SUPPLY, false));
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

    assert(!api->profile_write("modem0", NULL));
    assert(fake.clear_count == 1U);
    assert(solar_os_modem_profile_get("modem0", &loaded) == ESP_ERR_NOT_FOUND);
    assert(solar_os_modem_unregister("modem0") == ESP_OK);
    assert(solar_os_modem_count() == 0U);
    const unsigned rate_set_count = fake.transport_rate_set_count;
    assert(solar_os_modem_register(&registration) == ESP_OK);
    assert(fake.transport_rate_set_count == rate_set_count + 1U);
    assert(fake.configured_transport_rate == 230400U);
    assert(solar_os_modem_unregister("modem0") == ESP_OK);

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
    assert(!info.transport_rate_control);
    assert(solar_os_modem_get_status("modem1", &status) == ESP_OK);
    assert(!status.power_control && status.powered && status.online);
    assert(solar_os_modem_set_power("modem1", false) ==
           ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_modem_reset("modem1") == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_modem_transport_rate_get("modem1", &rate_info) ==
           ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_modem_transport_rate_set("modem1", 115200U) ==
           ESP_ERR_NOT_SUPPORTED);
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

    test_native_provider(api, &fake);
    puts("Modem service registry/profile and native lifetimes: ok");
    return 0;
}
