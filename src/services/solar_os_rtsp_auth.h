#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_RTSP_AUTH_HOST_MAX 63U
#define SOLAR_OS_RTSP_AUTH_USER_MAX 64U
#define SOLAR_OS_RTSP_AUTH_PASSWORD_MAX 64U
#define SOLAR_OS_RTSP_AUTH_CAPACITY 4U
#define SOLAR_OS_RTSP_AUTH_HEADER_MAX 384U

typedef struct {
    bool digest;
    bool qop_auth;
    bool stale;
    char realm[64];
    char nonce[128];
    char opaque[128];
    char cnonce[17];
    uint32_t nc;
} solar_os_rtsp_auth_challenge_t;

typedef struct {
    char host[SOLAR_OS_RTSP_AUTH_HOST_MAX + 1U];
    uint16_t port;
    char user[SOLAR_OS_RTSP_AUTH_USER_MAX + 1U];
} solar_os_rtsp_auth_account_t;

/* Parse one WWW-Authenticate value. Digest is preferred over Basic. */
esp_err_t solar_os_rtsp_auth_challenge(const char *www_authenticate,
                                      solar_os_rtsp_auth_challenge_t *challenge);
/* Authorization header value, without the header name. cnonce/nc are used
 * only for Digest qop=auth; pass NULL/0 to use the challenge copies. */
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
size_t solar_os_rtsp_auth_list(solar_os_rtsp_auth_account_t *records, size_t max_records);
void solar_os_rtsp_auth_wipe(void *buffer, size_t length);
