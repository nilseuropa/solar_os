#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "solar_os_native_rtsp_auth_abi.h"

/* Parse one WWW-Authenticate value. Digest is preferred over Basic. */
esp_err_t solar_os_rtsp_auth_challenge(const char *www_authenticate,
                                      solar_os_rtsp_auth_challenge_t *challenge);
/* Format the value of the Authorization header. For Digest qop=auth,
 * set cnonce and nc in the challenge before generating the value. */
esp_err_t solar_os_rtsp_authorization(const solar_os_rtsp_auth_challenge_t *challenge,
                                     const char *method,
                                     const char *uri,
                                     const char *user,
                                     const char *password,
                                     char *header,
                                     size_t capacity);

esp_err_t solar_os_rtsp_auth_set(const char *host, uint16_t port,
                                const char *user, const char *password);
esp_err_t solar_os_rtsp_auth_clear(const char *host, uint16_t port);
esp_err_t solar_os_rtsp_auth_lookup(const char *host, uint16_t port,
                                   char *user, size_t user_cap,
                                   char *password, size_t password_cap);
esp_err_t solar_os_rtsp_auth_list(solar_os_rtsp_auth_account_t *records, size_t capacity, size_t *count);
void solar_os_rtsp_auth_wipe(void *buffer, size_t length);
