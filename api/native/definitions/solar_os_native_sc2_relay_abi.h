#pragma once
#include "solar_os_native_sc2_profile_abi.h"

#define SOLAR_OS_NATIVE_SC2_RELAY_SERVICE "messaging.sc2.relay"
#define SOLAR_OS_NATIVE_SC2_RELAY_ABI 1U

#define SOLAR_OS_NATIVE_SC2_ROOM_CREATE 1U
#define SOLAR_OS_NATIVE_SC2_ROOM_INVITE 2U
#define SOLAR_OS_NATIVE_SC2_ROOM_JOIN 3U
#define SOLAR_OS_NATIVE_SC2_ROOM_LIST 4U
#define SOLAR_OS_NATIVE_SC2_ROOM_ID_CAPACITY 33U
#define SOLAR_OS_NATIVE_SC2_ROOM_OUT_CAPACITY 2048U

typedef struct {
    bool running, connected, ready;
    uint32_t instance, outbox, attempt;
    char relay_id[SOLAR_OS_NATIVE_SC2_RELAY_ID_CAPACITY];
    char host[128];
    uint32_t port;
    bool tls;
    uint32_t relay, relays, home;
    bool visiting;
} solar_os_native_sc2_relay_status_v1_t;

typedef struct {
    char url[128];
    uint8_t key[SOLAR_OS_NATIVE_SC2_RELAY_KEY_BYTES];
} solar_os_native_sc2_discovered_fallback_v1_t;

typedef struct {
    char relay_id[SOLAR_OS_NATIVE_SC2_RELAY_ID_CAPACITY];
    uint8_t key[SOLAR_OS_NATIVE_SC2_RELAY_KEY_BYTES];
    char host[64];
    uint16_t port;
    char pin[72];
    uint32_t fallback_count;
    solar_os_native_sc2_discovered_fallback_v1_t fallback[SOLAR_OS_NATIVE_SC2_RELAY_CAPACITY - 1U];
    uint64_t timestamp;
} solar_os_native_sc2_discovery_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    /* Shared relay worker serving the first enabled SC2 Messaging instance.
     * Stop may time out while a network request drains and may be retried. */
    int (*start)(void);
    int (*stop)(void);
    int (*status)(solar_os_native_sc2_relay_status_v1_t *status);
    /* The optional room buffer holds a room ID. The output contains an
     * invitation for create/invite, or room list for list. JOIN takes an
     * invitation, INVITE a room ID/prefix/name. A timed-out create can still
     * complete on the relay. Securely erase invitation text when finished. */
    int (*room)(uint32_t operation, const char *argument, char *room, size_t room_capacity,
        char *out, size_t out_capacity, char *reason, size_t reason_capacity, uint32_t timeout_ms);
    bool (*domain_valid)(const char *domain);
    /* Fetch over verified HTTPS and verify the signed relay descriptor.
     * An optional expected key points to SOLAR_OS_NATIVE_SC2_RELAY_KEY_BYTES.
     * This call discovers an endpoint without modifying a profile. */
    int (*discover)(const char *domain, const uint8_t *expected_key, solar_os_native_sc2_discovery_v1_t *descriptor,
        char *reason, size_t reason_capacity);
} solar_os_native_sc2_relay_api_v1_t;
