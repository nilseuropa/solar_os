#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_IRC_SERVICE "messages.irc"
#define SOLAR_OS_NATIVE_IRC_ABI 1U
#define SOLAR_OS_IRC_CHANNEL_CAPACITY 8U
#define SOLAR_OS_IRC_TARGET_MAX 64U

typedef struct {
    char host[128];
    char nick[32];
    char account[64];
    char ca_file[160];
    char channels[SOLAR_OS_IRC_CHANNEL_CAPACITY][SOLAR_OS_IRC_TARGET_MAX];
    uint32_t port;
    bool enabled, sasl_password_set, server_password_set;
} solar_os_native_irc_profile_v1_t;

/* Null strings keep the setting. Empty optional strings clear it. A zero
 * port keeps the existing port. A replacement channel list can be empty. */
typedef struct {
    const char *host, *nick, *account, *password, *server_password, *ca_file;
    const char *const *channels;
    size_t channel_count;
    uint32_t port;
    bool replace_channels, set_enabled, enabled;
} solar_os_native_irc_update_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    /* Use messages.provider_add with class "irc" to create a provider. The
     * profile includes password presence flags and the saved channel list. */
    int (*profile)(uint32_t provider, solar_os_native_irc_profile_v1_t *out);
    int (*configure)(uint32_t provider, const solar_os_native_irc_update_v1_t *update);
    /* One shared worker serves all configured IRC instances, including the
     * built-in irc-sync job. A timed-out stop can be retried. */
    int (*start)(void);
    int (*stop)(void);
    bool (*running)(void);
} solar_os_native_irc_api_v1_t;
