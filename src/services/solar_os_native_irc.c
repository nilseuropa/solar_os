#include "solar_os_native_irc.h"
#include "solar_os_config.h"

#if SOLAR_OS_PACKAGE_SERVICE_IRC
#include "solar_os_irc.h"
#include "solar_os_memory.h"
#include "solar_os_native_support.h"

#include <string.h>

static int irc_profile(uint32_t provider, solar_os_native_irc_profile_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_irc_profile_v1_t){0};
    solar_os_irc_config_t *config = solar_os_memory_calloc(
        1, sizeof(*config), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "native.irc.profile");
    if (!config) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    const esp_err_t error = solar_os_irc_get_config(provider, config);
    if (error == ESP_OK) {
#define COPY(field) \
        _Static_assert(sizeof(out->field) == sizeof(config->field), #field " capacity"); \
        memcpy(out->field, config->field, sizeof(out->field)); \
        out->field[sizeof(out->field) - 1U] = '\0'
        COPY(host);
        COPY(nick);
        COPY(account);
        COPY(ca_file);
        _Static_assert(sizeof(out->channels) == sizeof(config->channels), "channel capacity");
        memcpy(out->channels, config->channels, sizeof(out->channels));
        for (size_t i = 0; i < SOLAR_OS_IRC_CHANNEL_CAPACITY; i++)
            out->channels[i][SOLAR_OS_IRC_TARGET_MAX - 1U] = '\0';
#undef COPY
        out->port = config->port;
        out->enabled = config->enabled;
        out->sasl_password_set = config->password[0] != '\0';
        out->server_password_set = config->server_password[0] != '\0';
    }
    explicit_bzero(config, sizeof(*config));
    solar_os_memory_free(config);
    return solar_os_native_result(error);
}

static int irc_configure(uint32_t provider, const solar_os_native_irc_update_v1_t *update)
{
    return solar_os_native_result(solar_os_irc_update(provider, update));
}

static int irc_start(void)
{
    const esp_err_t error = solar_os_irc_start();
    return error == ESP_ERR_INVALID_STATE ? SOLAR_OS_NATIVE_ERROR_BUSY : solar_os_native_result(error);
}

static int irc_stop(void)
{
    return solar_os_native_result(solar_os_irc_stop_checked());
}

static const solar_os_native_irc_api_v1_t irc_api = {
    .abi_version = SOLAR_OS_NATIVE_IRC_ABI,
    .struct_size = sizeof(irc_api),
    .profile = irc_profile,
    .configure = irc_configure,
    .start = irc_start,
    .stop = irc_stop,
    .running = solar_os_irc_running,
};
#endif

const void *solar_os_native_irc_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_irc_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
