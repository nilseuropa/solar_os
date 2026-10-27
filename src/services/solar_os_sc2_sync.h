#pragma once

/*
 * sc2-sync: the relay transport for the "sc2" Messaging provider. One worker
 * task drives the first enabled sc2 instance over TCP (chat://) or TLS
 * (chats://: the CA bundle, or with #pin= the SPKI pin alone) with the sans-IO client in
 * solar_os_sc2_client.c: E2E DMs and signed public rooms (#general), with
 * reconnect backoff. One connection at a time: after two failed attempts it
 * moves to the next configured relay (fallback), probes the primary and
 * visits fallbacks every 15 minutes (keys, mailbox). Private rooms
 * (solar_os_sc2_sync_room) live on the primary relay only. Transport: esp_tls (TCP/TLS); the client is sans-IO,
 * so another link (e.g. a relay-side WebSocket) only replaces try_connect,
 * h_send and pump_input.
 */

#include <stdbool.h>
#include "esp_err.h"

/* Worker stack (internal RAM); the job descriptor uses it for admission. */
#define SOLAR_OS_SC2_SYNC_STACK 12288u

esp_err_t solar_os_sc2_sync_start(void);
void solar_os_sc2_sync_stop(void);
/* Install the provider hook so enabling an instance starts the worker. */
void solar_os_sc2_sync_install(void);

typedef struct {
    bool running;
    bool connected;
    bool ready;
    unsigned instance;
    unsigned outbox;
    unsigned attempt;
    char relay_id[64];
    char host[128]; /* SOLAR_OS_SC2_RELAY_URL_MAX */
    unsigned port;
    bool tls;
    unsigned relay;  /* in use: 0 primary, 1.. fallback */
    unsigned relays; /* configured: primary + fallbacks */
    unsigned home;   /* relay to stay on (the primary unless it failed) */
    bool visiting;   /* short visit: keys and mailbox, then back home */
} solar_os_sc2_sync_status_t;

void solar_os_sc2_sync_get_status(solar_os_sc2_sync_status_t *status);

/* Private rooms (shell side). The worker runs the request between two
 * relay reads; the call waits up to timeout_ms for it. */
typedef enum {
    SOLAR_OS_SC2_ROOM_CREATE = 1, /* new room, this device admin; out = invite text */
    SOLAR_OS_SC2_ROOM_INVITE,     /* arg = room (hex or unique prefix); out = invite text */
    SOLAR_OS_SC2_ROOM_JOIN,       /* arg = invite text "sc2i:..." */
    SOLAR_OS_SC2_ROOM_LIST,       /* out = one line per room */
} solar_os_sc2_room_op_t;

#define SOLAR_OS_SC2_ROOM_OUT_MAX 2048u

/* room (33 bytes, may be NULL) gets the room key; out (cap bytes, may be
 * NULL) the text; why (whycap) a reason when the result is not ESP_OK. */
esp_err_t solar_os_sc2_sync_room(solar_os_sc2_room_op_t op, const char *arg, char *room, char *out, size_t cap,
                                 char *why, size_t whycap, uint32_t timeout_ms);
