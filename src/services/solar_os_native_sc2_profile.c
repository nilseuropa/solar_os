#include "solar_os_native_sc2_profile.h"
#include "solar_os_config.h"

#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_SC2_PROVIDER
#include "sc2_relaykey.h"
#include "solar_os_memory.h"
#include "solar_os_native_sc2_abi.h"
#include "solar_os_native_support.h"
#include "solar_os_sc2_provider.h"

_Static_assert(SOLAR_OS_NATIVE_SC2_RELAY_CAPACITY == SOLAR_OS_SC2_RELAY_FALLBACK_MAX + 1U &&
    SOLAR_OS_NATIVE_SC2_SUBSCRIPTIONS_MAX == SOLAR_OS_SC2_RELAY_ROOM_CAPACITY &&
    SOLAR_OS_NATIVE_SC2_RELAY_KEY_BYTES == SOLAR_OS_SC2_RELAY_KEY_LEN &&
    SOLAR_OS_NATIVE_SC2_RELAY_ID_CAPACITY == SC2_RELAY_ID_MAX + 1U, "relay profile capacities");
_Static_assert(SOLAR_OS_NATIVE_SC2_TARGET_DEVICE == SOLAR_OS_SC2_TARGET_DEVICE &&
    SOLAR_OS_NATIVE_SC2_TARGET_PRIVATE_ROOM == SOLAR_OS_SC2_TARGET_PRIVATE_ROOM &&
    SOLAR_OS_NATIVE_SC2_TARGET_PUBLIC_ROOM == SOLAR_OS_SC2_TARGET_PUBLIC_ROOM &&
    SOLAR_OS_NATIVE_SC2_TARGET_INVALID == SOLAR_OS_SC2_TARGET_INVALID, "relay target values");

static solar_os_sc2_relay_config_t *allocate_snapshot(void)
{
    return solar_os_memory_calloc(1, sizeof(solar_os_sc2_relay_config_t),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "native.sc2.profile");
}

static void free_snapshot(solar_os_sc2_relay_config_t *config)
{
    explicit_bzero(config, sizeof(*config));
    solar_os_memory_free(config);
}

static int profile_read(solar_os_messaging_provider_id_t id, solar_os_native_sc2_profile_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    solar_os_sc2_relay_config_t *config = allocate_snapshot();
    if (!config) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    const esp_err_t error = solar_os_sc2_relay_get_config(id, config);
    if (error == ESP_OK) {
        out->enabled = config->enabled;
        out->admission_configured = config->admit[0] != '\0';
        _Static_assert(sizeof(out->rooms) == sizeof(config->rooms), "relay room capacity");
        memcpy(out->rooms, config->rooms, sizeof(out->rooms));
        out->relay_count = (uint32_t)solar_os_sc2_relay_endpoint(config, 0, NULL);
        for (size_t i = 0; i < out->relay_count; ++i) {
            solar_os_native_sc2_peer_relay_v1_t *relay = &out->relays[i];
            (void)solar_os_sc2_relay_endpoint(config, i, &relay->endpoint);
            relay->key_pinned = solar_os_sc2_relay_key(config, i, relay->key);
        }
    }
    free_snapshot(config);
    return solar_os_native_result(error);
}

static int parse_endpoint(const char *url, solar_os_native_sc2_endpoint_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_native_sc2_endpoint_v1_t endpoint = {0};
    const esp_err_t error = solar_os_sc2_relay_parse_endpoint(url, &endpoint);
    *out = endpoint;
    return solar_os_native_result(error);
}

static uint32_t normalize_target(const char *target, bool room, char *out, size_t capacity)
{
    const uint32_t kind = solar_os_sc2_target_normalize(target, room, out, capacity);
    if (!kind && out && capacity) out[0] = '\0';
    return kind;
}

static void room_title(char *out, size_t capacity, const char *key)
{
    if (!out || !capacity) return;
    solar_os_sc2_room_title(out, capacity, key);
}

static int security_flags(solar_os_messaging_provider_id_t id, uint32_t kind, bool peer_key_known, uint32_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (kind == SOLAR_OS_NATIVE_SC2_TARGET_INVALID || kind > SOLAR_OS_NATIVE_SC2_TARGET_PUBLIC_ROOM)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_sc2_relay_config_t *config = allocate_snapshot();
    if (!config) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    const esp_err_t error = solar_os_sc2_relay_get_config(id, config);
    if (error == ESP_OK) *out = solar_os_sc2_conversation_security(config, (solar_os_sc2_target_kind_t)kind, peer_key_known);
    free_snapshot(config);
    return solar_os_native_result(error);
}

static int rotate_key_for_relay(solar_os_messaging_provider_id_t id, size_t relay, const char *relay_id,
    const solar_os_native_sc2_rotation_v1_t *proof)
{
    if (!proof || relay >= SOLAR_OS_NATIVE_SC2_RELAY_CAPACITY || !relay_id || !relay_id[0] ||
        strnlen(relay_id, SOLAR_OS_NATIVE_SC2_RELAY_ID_CAPACITY) >= SOLAR_OS_NATIVE_SC2_RELAY_ID_CAPACITY)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (sc2_rotation_verify(proof->rotation_signature, proof->old_key, relay_id, proof->new_key) != SC2_OK ||
        sc2_welcome_verify(proof->welcome_signature, proof->new_key, relay_id, proof->nonce, proof->challenge) != SC2_OK)
        return SOLAR_OS_NATIVE_SC2_ERROR_AUTH;
    return solar_os_native_result(solar_os_sc2_relay_set_key(id, relay, proof->old_key, proof->new_key));
}

static int rotate_key(solar_os_messaging_provider_id_t id, size_t relay, const solar_os_native_sc2_rotation_v1_t *proof)
{
    if (!proof || !memchr(proof->relay_id, 0, sizeof(proof->relay_id))) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return rotate_key_for_relay(id, relay, proof->relay_id, proof);
}

static const solar_os_native_sc2_profile_api_v1_t sc2_profile_api = {
    .abi_version = SOLAR_OS_NATIVE_SC2_PROFILE_ABI,
    .struct_size = sizeof(sc2_profile_api),
    .read = profile_read, .parse_endpoint = parse_endpoint, .normalize_target = normalize_target,
    .room_title = room_title, .security_flags = security_flags, .rotate_key = rotate_key,
    .rotate_key_for_relay = rotate_key_for_relay,
};
#endif

const void *solar_os_native_sc2_profile_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_sc2_profile_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
