#pragma once
#include "can_twai.h"
#include "solar_os_expansion.h"

typedef struct {
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    unsigned bitrate_kbit;
    bool listen_only;
} solar_os_can_info_t;
esp_err_t solar_os_can_attach(const char *, const solar_os_expansion_binding_t *, size_t);
esp_err_t solar_os_can_detach(const char *);
bool solar_os_can_get_info(solar_os_can_info_t *);
esp_err_t solar_os_can_send(const char *, const solar_os_can_frame_t *, uint32_t timeout_ms);
esp_err_t solar_os_can_receive(const char *, solar_os_can_frame_t *, uint32_t timeout_ms);
esp_err_t solar_os_can_status(const char *, solar_os_can_status_t *);
esp_err_t solar_os_can_recover(const char *);
esp_err_t solar_os_can_start(const char *);
