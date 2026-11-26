#include "solar_os_native_sc2_relay.h"

#include <stdio.h>
#include <string.h>

#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_SERVICE_SC2_RELAY
#include "solar_os_memory.h"
#include "solar_os_native_support.h"
#include "solar_os_sc2_discover.h"
#include "solar_os_sc2_sync.h"

_Static_assert(SOLAR_OS_NATIVE_SC2_ROOM_ID_CAPACITY == SC2_DEVICE_ID_HEX_LEN + 1U, "SC2 room ID capacity");
_Static_assert(SOLAR_OS_NATIVE_SC2_RELAY_ID_CAPACITY == SC2_RELAY_ID_MAX + 1U &&
    SOLAR_OS_NATIVE_SC2_RELAY_KEY_BYTES == SC2_KEY_LEN &&
    SOLAR_OS_NATIVE_SC2_RELAY_CAPACITY == SC2_DESC_FALLBACK_MAX + 1U, "SC2 relay descriptor capacities");

static int start(void)
{
    return solar_os_native_result(solar_os_sc2_sync_start());
}

static int stop(void)
{
    return solar_os_native_result(solar_os_sc2_sync_stop_checked());
}

static int status(solar_os_native_sc2_relay_status_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_sc2_sync_get_status(out);
    return 0;
}

static int room(uint32_t op, const char *arg, char *id, size_t id_cap, char *out, size_t out_cap,
    char *why, size_t why_cap, uint32_t timeout_ms)
{
    if (op < SOLAR_OS_NATIVE_SC2_ROOM_CREATE || op > SOLAR_OS_NATIVE_SC2_ROOM_LIST ||
        (id && id_cap < SOLAR_OS_NATIVE_SC2_ROOM_ID_CAPACITY) || (!id && id_cap) ||
        (!out && out_cap) || (!why && why_cap)) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    if (id) id[0] = '\0';
    if (out && out_cap) out[0] = '\0';
    return solar_os_native_result(solar_os_sc2_sync_room((solar_os_sc2_room_op_t)op, arg, id, out, out_cap,
        why, why_cap, timeout_ms));
}

static int discover(const char *domain, const uint8_t *expect, solar_os_native_sc2_discovery_v1_t *out,
    char *why, size_t why_cap)
{
    if (!out || (!why && why_cap)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (why && why_cap) why[0] = '\0';
    sc2_desc_t *descriptor = solar_os_memory_alloc(sizeof(*descriptor), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "native.sc2.discover");
    if (!descriptor) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    const esp_err_t error = solar_os_sc2_discover(domain, expect, descriptor, why_cap ? why : NULL, why_cap);
    if (error == ESP_OK) {
        memcpy(out->relay_id, descriptor->relay_id, sizeof(out->relay_id));
        memcpy(out->key, descriptor->key, sizeof(out->key));
        memcpy(out->host, descriptor->host, sizeof(out->host));
        if (out->host[0] == '\0') snprintf(out->host, sizeof(out->host), "%s", domain);
        memcpy(out->pin, descriptor->pin, sizeof(out->pin));
        out->port = descriptor->port;
        out->timestamp = descriptor->ts;
        out->fallback_count = descriptor->fallback_count;
        for (size_t i = 0; i < out->fallback_count; ++i) {
            memcpy(out->fallback[i].url, descriptor->fallback[i].url, sizeof(out->fallback[i].url));
            memcpy(out->fallback[i].key, descriptor->fallback[i].key, sizeof(out->fallback[i].key));
        }
    }
    solar_os_memory_free(descriptor);
    return solar_os_native_result(error);
}

static const solar_os_native_sc2_relay_api_v1_t sc2_relay_api = {
    .abi_version = SOLAR_OS_NATIVE_SC2_RELAY_ABI,
    .struct_size = sizeof(sc2_relay_api),
    .start = start,
    .stop = stop,
    .status = status,
    .room = room,
    .domain_valid = solar_os_sc2_discover_domain_valid,
    .discover = discover,
};
#endif

const void *solar_os_native_sc2_relay_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_sc2_relay_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
