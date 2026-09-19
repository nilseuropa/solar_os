#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#define SOLAR_OS_PPP_NAME_MAX 20U
#define SOLAR_OS_PPP_USERNAME_MAX 64U
#define SOLAR_OS_PPP_PASSWORD_MAX 64U
#define SOLAR_OS_PPP_ADDRESS_MAX 46U
#define SOLAR_OS_PPP_INTERFACE_MAX 8U

typedef enum {
    SOLAR_OS_PPP_AUTH_NONE = 0,
    SOLAR_OS_PPP_AUTH_PAP,
    SOLAR_OS_PPP_AUTH_CHAP,
} solar_os_ppp_auth_t;

typedef enum {
    SOLAR_OS_PPP_STATE_DOWN = 0,
    SOLAR_OS_PPP_STATE_CONNECTING,
    SOLAR_OS_PPP_STATE_UP,
    SOLAR_OS_PPP_STATE_FAILED,
} solar_os_ppp_state_t;

typedef struct {
    solar_os_ppp_auth_t auth;
    char username[SOLAR_OS_PPP_USERNAME_MAX + 1U];
    char password[SOLAR_OS_PPP_PASSWORD_MAX + 1U];
    char dns[SOLAR_OS_PPP_ADDRESS_MAX];
} solar_os_ppp_profile_t;

typedef struct {
    solar_os_ppp_state_t state;
    int32_t error;
    char interface_name[SOLAR_OS_PPP_INTERFACE_MAX];
    char ipv4_address[SOLAR_OS_PPP_ADDRESS_MAX];
    char ipv4_gateway[SOLAR_OS_PPP_ADDRESS_MAX];
    char dns_address[SOLAR_OS_PPP_ADDRESS_MAX];
} solar_os_ppp_status_t;

typedef struct {
    /* Optional hooks prepare and restore a link around each PPP session. */
    esp_err_t (*start)(void *ctx);
    esp_err_t (*stop)(void *ctx);
    /* Read is optional for transports that call solar_os_ppp_receive(). */
    esp_err_t (*read)(void *ctx,
                      uint8_t *data,
                      size_t length,
                      uint32_t timeout_ms,
                      size_t *read_length);
    esp_err_t (*write)(void *ctx,
                       const uint8_t *data,
                       size_t length,
                       size_t *written);
    void *ctx;
} solar_os_ppp_transport_t;

typedef struct {
    /* Optional hooks bind the PPP netif to an owner-defined network role. */
    esp_err_t (*attach)(void *ctx, esp_netif_t *netif);
    void (*set_ready)(void *ctx, esp_netif_t *netif, bool ready);
    void (*detach)(void *ctx, esp_netif_t *netif);
    void *ctx;
} solar_os_ppp_netif_binding_t;

typedef struct {
    const char *name;
    /* Pull transports must return from read within this interval. */
    uint32_t read_timeout_ms;
    solar_os_ppp_transport_t transport;
    solar_os_ppp_netif_binding_t netif;
} solar_os_ppp_config_t;

typedef struct solar_os_ppp solar_os_ppp_t;

esp_err_t solar_os_ppp_create(const solar_os_ppp_config_t *config,
                              solar_os_ppp_t **out_ppp);
esp_err_t solar_os_ppp_destroy(solar_os_ppp_t *ppp);

esp_err_t solar_os_ppp_connect(solar_os_ppp_t *ppp,
                               const solar_os_ppp_profile_t *profile,
                               uint32_t timeout_ms);
esp_err_t solar_os_ppp_disconnect(solar_os_ppp_t *ppp);

/* Reconcile a PPP session after its physical transport has been reset. */
esp_err_t solar_os_ppp_notify_transport_reset(solar_os_ppp_t *ppp);

/* Feed received bytes here when the transport does not provide read(). */
esp_err_t solar_os_ppp_receive(solar_os_ppp_t *ppp,
                               const uint8_t *data,
                               size_t length);

esp_err_t solar_os_ppp_get_status(solar_os_ppp_t *ppp,
                                  solar_os_ppp_status_t *status);
bool solar_os_ppp_is_busy(solar_os_ppp_t *ppp);
bool solar_os_ppp_is_connected(solar_os_ppp_t *ppp);
