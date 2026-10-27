#pragma once
#include "esp_err.h"
#include "solar_os_messaging.h"

#define SOLAR_OS_IRC_CHANNEL_CAPACITY 8U
#define SOLAR_OS_IRC_TARGET_MAX 64U
typedef struct {
    uint32_t version;
    bool enabled;
    uint16_t port;
    char host[128];
    char nick[32];
    char account[64];
    char password[128];
    char server_password[128];
    char ca_file[160];
    char channels[SOLAR_OS_IRC_CHANNEL_CAPACITY][SOLAR_OS_IRC_TARGET_MAX];
} solar_os_irc_config_t;

esp_err_t solar_os_irc_init(void);
esp_err_t solar_os_irc_create(const char *name, const char *host, uint16_t port,
                             const char *nick, solar_os_messaging_provider_id_t *id);
esp_err_t solar_os_irc_get_config(solar_os_messaging_provider_id_t id, solar_os_irc_config_t *config);
esp_err_t solar_os_irc_configure(solar_os_messaging_provider_id_t id, const solar_os_irc_config_t *config);
esp_err_t solar_os_irc_enable(solar_os_messaging_provider_id_t id, bool enabled);
esp_err_t solar_os_irc_start(void);
void solar_os_irc_stop(void);
