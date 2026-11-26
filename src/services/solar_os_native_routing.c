#include "solar_os_native_routing.h"
#include "solar_os_native_routing_abi.h"
#include "solar_os_native_wifi_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_config.h"
#include "solar_os_memory.h"
#include <stdio.h>
#include <string.h>
#define COPY_FIELD(out, in, field) do { \
    _Static_assert(sizeof((out)->field) == sizeof((in)->field), "native field capacity changed"); \
    memcpy((out)->field, (in)->field, sizeof((out)->field)); \
} while (0)
#if SOLAR_OS_PACKAGE_SERVICE_NETWORK
#include "solar_os_network.h"
static void path_out(const solar_os_network_path_info_t *in, solar_os_native_network_path_v1_t *out)
{
    memset(out, 0, sizeof(*out)); COPY_FIELD(out, in, name);
    out->priority = in->route_priority; out->ready = in->ready; out->connecting = in->connecting;
    if (in->dns.ip.type == ESP_IPADDR_TYPE_V4) {
        out->dns_family = 4; memcpy(out->dns_address, &in->dns.ip.u_addr.ip4.addr, 4);
    } else if (in->dns.ip.type == ESP_IPADDR_TYPE_V6) {
        out->dns_family = 6; out->dns_zone = in->dns.ip.u_addr.ip6.zone;
        memcpy(out->dns_address, in->dns.ip.u_addr.ip6.addr, 16);
    }
}
static int paths(solar_os_native_network_path_v1_t *out, size_t capacity, size_t *count)
{
    if (!count || (!out && capacity)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_network_path_info_t values[SOLAR_OS_NETWORK_PATH_MAX] = {0};
    size_t copied = capacity < SOLAR_OS_NETWORK_PATH_MAX ? capacity : SOLAR_OS_NETWORK_PATH_MAX;
    *count = solar_os_network_path_list(values, copied);
    for (size_t i = 0; i < *count && i < capacity; ++i) path_out(&values[i], &out[i]);
    return SOLAR_OS_NATIVE_OK;
}
static int preferred(solar_os_native_network_path_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_network_path_info_t value = {0};
    if (!solar_os_network_path_get_preferred(&value)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    path_out(&value, out); return SOLAR_OS_NATIVE_OK;
}
static int priority_set(const char *name, int32_t priority)
{ return solar_os_native_result(solar_os_network_path_set_priority(name, priority)); }
static int role_out(solar_os_network_interface_role_t role)
{
    switch (role) {
    case SOLAR_OS_NETWORK_INTERFACE_ROLE_UPLINK: return SOLAR_OS_NATIVE_ROUTING_UPLINK;
    case SOLAR_OS_NETWORK_INTERFACE_ROLE_DOWNSTREAM: return SOLAR_OS_NATIVE_ROUTING_DOWNSTREAM;
    case SOLAR_OS_NETWORK_INTERFACE_ROLE_PEER: return SOLAR_OS_NATIVE_ROUTING_PEER;
    case SOLAR_OS_NETWORK_INTERFACE_ROLE_TUNNEL: return SOLAR_OS_NATIVE_ROUTING_TUNNEL;
    default: return -1;
    }
}
static int state_out(solar_os_network_interface_state_t state)
{
    switch (state) {
    case SOLAR_OS_NETWORK_INTERFACE_STATE_DOWN: return SOLAR_OS_NATIVE_ROUTING_DOWN;
    case SOLAR_OS_NETWORK_INTERFACE_STATE_CONNECTING: return SOLAR_OS_NATIVE_ROUTING_CONNECTING;
    case SOLAR_OS_NETWORK_INTERFACE_STATE_UP: return SOLAR_OS_NATIVE_ROUTING_UP;
    case SOLAR_OS_NETWORK_INTERFACE_STATE_ERROR: return SOLAR_OS_NATIVE_ROUTING_ERROR;
    default: return -1;
    }
}
static int interfaces(solar_os_native_network_interface_v1_t *out, size_t capacity, size_t *count)
{
    if (!count || (!out && capacity)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (capacity > SOLAR_OS_NETWORK_INTERFACE_MAX) capacity = SOLAR_OS_NETWORK_INTERFACE_MAX;
    solar_os_network_interface_info_t *values = capacity ? solar_os_memory_calloc(capacity, sizeof(*values),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.routing") : NULL;
    *count = 0; if (capacity && !values) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    *count = solar_os_network_interface_list(values, capacity);
    int result = SOLAR_OS_NATIVE_OK;
    for (size_t i = 0; i < *count && i < capacity; ++i) {
        int role = role_out(values[i].role), state = state_out(values[i].state);
        if (role < 0 || state < 0) { result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE; break; }
        out[i] = (solar_os_native_network_interface_v1_t){.priority = values[i].route_priority,
            .error = solar_os_native_result(values[i].last_error), .role = role, .state = state, .nat_enabled = values[i].nat_enabled};
        COPY_FIELD(&out[i], &values[i], name); COPY_FIELD(&out[i], &values[i], address); COPY_FIELD(&out[i], &values[i], peer);
    }
    solar_os_memory_free(values); return result;
}
static int interface_publish(const solar_os_native_network_interface_v1_t *in)
{
    if (!in || !memchr(in->name, 0, sizeof(in->name)) || !memchr(in->address, 0, sizeof(in->address)) ||
        !memchr(in->peer, 0, sizeof(in->peer)) || in->nat_enabled > 1) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_network_interface_info_t out = {.route_priority = in->priority, .nat_enabled = in->nat_enabled != 0};
    switch (in->error) {
    case SOLAR_OS_NATIVE_OK: out.last_error = ESP_OK; break;
    case SOLAR_OS_NATIVE_ERROR_FAILED: out.last_error = ESP_FAIL; break;
    case SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT: out.last_error = ESP_ERR_INVALID_ARG; break;
    case SOLAR_OS_NATIVE_ERROR_INVALID_STATE: out.last_error = ESP_ERR_INVALID_STATE; break;
    case SOLAR_OS_NATIVE_ERROR_NO_MEMORY: out.last_error = ESP_ERR_NO_MEM; break;
    case SOLAR_OS_NATIVE_ERROR_NOT_FOUND: out.last_error = ESP_ERR_NOT_FOUND; break;
    case SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED: out.last_error = ESP_ERR_NOT_SUPPORTED; break;
    case SOLAR_OS_NATIVE_ERROR_TIMEOUT: out.last_error = ESP_ERR_TIMEOUT; break;
    case SOLAR_OS_NATIVE_ERROR_INVALID_SIZE: out.last_error = ESP_ERR_INVALID_SIZE; break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    switch (in->role) {
    case SOLAR_OS_NATIVE_ROUTING_DOWNSTREAM: out.role = SOLAR_OS_NETWORK_INTERFACE_ROLE_DOWNSTREAM; break;
    case SOLAR_OS_NATIVE_ROUTING_PEER: out.role = SOLAR_OS_NETWORK_INTERFACE_ROLE_PEER; break;
    case SOLAR_OS_NATIVE_ROUTING_TUNNEL: out.role = SOLAR_OS_NETWORK_INTERFACE_ROLE_TUNNEL; break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    switch (in->state) {
    case SOLAR_OS_NATIVE_ROUTING_DOWN: out.state = SOLAR_OS_NETWORK_INTERFACE_STATE_DOWN; break;
    case SOLAR_OS_NATIVE_ROUTING_CONNECTING: out.state = SOLAR_OS_NETWORK_INTERFACE_STATE_CONNECTING; break;
    case SOLAR_OS_NATIVE_ROUTING_UP: out.state = SOLAR_OS_NETWORK_INTERFACE_STATE_UP; break;
    case SOLAR_OS_NATIVE_ROUTING_ERROR: out.state = SOLAR_OS_NETWORK_INTERFACE_STATE_ERROR; break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    COPY_FIELD(&out, in, name); COPY_FIELD(&out, in, address); COPY_FIELD(&out, in, peer);
    return solar_os_native_result(solar_os_network_interface_publish(&out));
}
static int interface_remove(const char *name) { return solar_os_native_result(solar_os_network_interface_remove(name)); }
static int router_status(solar_os_native_network_router_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_network_router_status_t value = {0}; solar_os_network_router_get_status(&value);
    *out = (solar_os_native_network_router_v1_t){.available = value.available, .downstream_active = value.downstream_active,
        .enabled = value.enabled, .active = value.active, .nat_enabled = value.nat_enabled,
        .client_count = value.client_count, .client_limit = value.client_limit, .error = solar_os_native_result(value.last_error)};
    COPY_FIELD(out, &value, downstream); COPY_FIELD(out, &value, address); COPY_FIELD(out, &value, label);
    return SOLAR_OS_NATIVE_OK;
}
static int router_start(void) { return solar_os_native_result(solar_os_network_router_start()); }
static int router_stop(void) { return solar_os_native_result(solar_os_network_router_stop()); }
static const solar_os_native_routing_api_v1_t routing_api = {
    .abi_version = SOLAR_OS_NATIVE_ROUTING_ABI, .struct_size = sizeof(routing_api),
    .paths = paths, .preferred = preferred, .priority_set = priority_set, .interfaces = interfaces,
    .interface_publish = interface_publish, .interface_remove = interface_remove,
    .router_status = router_status, .router_start = router_start, .router_stop = router_stop,
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
#include "solar_os_wifi.h"
static bool wifi_boot_enabled(bool next)
{ return next ? solar_os_wifi_enabled_for_next_boot() : solar_os_wifi_enabled_for_current_boot(); }
static int wifi_boot_enable(bool enabled) { return solar_os_native_result(solar_os_wifi_set_enabled_for_next_boot(enabled)); }
#define WIFI_NOARG(name, target) static int name(void) { return solar_os_native_result(target()); }
WIFI_NOARG(wifi_start, solar_os_wifi_start)
WIFI_NOARG(wifi_stop, solar_os_wifi_stop)
WIFI_NOARG(wifi_connect_saved, solar_os_wifi_connect_saved)
WIFI_NOARG(wifi_disconnect, solar_os_wifi_disconnect)
WIFI_NOARG(wifi_forget_all, solar_os_wifi_forget_all)
WIFI_NOARG(wifi_ap_stop, solar_os_wifi_ap_stop)
WIFI_NOARG(wifi_ap_forget, solar_os_wifi_ap_forget)
WIFI_NOARG(wifi_repeater_start, solar_os_wifi_repeater_start)
WIFI_NOARG(wifi_repeater_stop, solar_os_wifi_repeater_stop)
WIFI_NOARG(wifi_scan_start, solar_os_wifi_scan_start_async)
WIFI_NOARG(wifi_scan_cancel, solar_os_wifi_scan_cancel_async)
#undef WIFI_NOARG
static int wifi_connect(const char *ssid, const char *password) { return solar_os_native_result(solar_os_wifi_connect(ssid, password)); }
static int wifi_forget(const char *ssid) { return solar_os_native_result(ssid ? solar_os_wifi_forget_ssid(ssid) : solar_os_wifi_forget()); }
static int wifi_known(solar_os_native_wifi_profile_v1_t *out, size_t capacity, size_t *count)
{
    if (!count || (!out && capacity)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (capacity > SOLAR_OS_WIFI_PROFILE_MAX) capacity = SOLAR_OS_WIFI_PROFILE_MAX;
    solar_os_wifi_profile_t values[SOLAR_OS_WIFI_PROFILE_MAX] = {0}; *count = 0;
    int result = solar_os_native_result(solar_os_wifi_known(values, capacity, count));
    if (!result) for (size_t i = 0; i < *count && i < capacity; ++i) {
        memset(&out[i], 0, sizeof(out[i])); COPY_FIELD(&out[i], &values[i], ssid); out[i].preferred = values[i].preferred;
    }
    return result;
}
static int wifi_ap_start(const char *ssid, const char *password, const char *auth)
{ return solar_os_native_result(solar_os_wifi_ap_start(ssid, password, auth)); }
static int wifi_ap_save(const char *ssid, const char *password, const char *auth)
{ return solar_os_native_result(solar_os_wifi_ap_save(ssid, password, auth)); }
static int wifi_ap_saved(solar_os_native_wifi_ap_config_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_wifi_ap_config_t value = {0};
    int result = solar_os_native_result(solar_os_wifi_ap_saved_get(&value));
    if (!result) { COPY_FIELD(out, &value, ssid); COPY_FIELD(out, &value, password); COPY_FIELD(out, &value, auth); }
    return result;
}
static int wifi_nat_set(bool enabled) { return solar_os_native_result(solar_os_wifi_nat_set(enabled)); }
static int wifi_scan_common(solar_os_native_wifi_ap_v1_t *out, size_t capacity, size_t *count, bool results)
{
    if (!count || (!out && capacity)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (capacity > SOLAR_OS_WIFI_SCAN_MAX_RESULTS) capacity = SOLAR_OS_WIFI_SCAN_MAX_RESULTS;
    solar_os_wifi_ap_t values[SOLAR_OS_WIFI_SCAN_MAX_RESULTS] = {0}; *count = 0;
    int result = solar_os_native_result(results ? solar_os_wifi_scan_results(values, capacity, count) : solar_os_wifi_scan(values, capacity, count));
    if (!result) for (size_t i = 0; i < *count && i < capacity; ++i) {
        memset(&out[i], 0, sizeof(out[i])); COPY_FIELD(&out[i], &values[i], ssid); COPY_FIELD(&out[i], &values[i], auth);
        out[i].rssi = values[i].rssi; out[i].channel = values[i].channel; out[i].hidden = values[i].hidden;
    }
    return result;
}
static int wifi_scan(solar_os_native_wifi_ap_v1_t *out, size_t capacity, size_t *count) { return wifi_scan_common(out, capacity, count, false); }
static int wifi_scan_results(solar_os_native_wifi_ap_v1_t *out, size_t capacity, size_t *count) { return wifi_scan_common(out, capacity, count, true); }
typedef struct { char owner[32]; bool latency, acquired; } native_wifi_lease_t;
static int wifi_lease_destroy(void *value)
{
    native_wifi_lease_t *lease = value;
    if (lease->acquired) {
        /* Both OS release methods consume the owner before applying the radio
         * policy. Even an error must not release again on a cleanup retry. */
        lease->acquired = false;
        int result = solar_os_native_result(lease->latency ? solar_os_wifi_latency_release(lease->owner) : solar_os_wifi_connectionless_release(lease->owner));
        if (result) return result;
    }
    solar_os_memory_free(lease); return SOLAR_OS_NATIVE_OK;
}
static int wifi_acquire(const char *owner, uint32_t channel, uint32_t *actual, bool latency, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0; if (actual) *actual = 0;
    if (!owner || !*owner || strlen(owner) >= 32 || channel > UINT8_MAX || (!latency && !actual)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_wifi_lease_t *lease = solar_os_memory_calloc(1, sizeof(*lease), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.wifi");
    if (!lease) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    lease->latency = latency;
    int result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_WIFI_LEASE, lease, wifi_lease_destroy, out);
    if (result) { wifi_lease_destroy(lease); return result; }
    /* The OS's string-keyed acquisitions are idempotent. Give every native
     * token a distinct key so closing one lease cannot release another. */
    snprintf(lease->owner, sizeof(lease->owner), "%08lx/%.22s", (unsigned long)*out, owner);
    uint8_t actual_channel = 0;
    result = solar_os_native_result(latency ? solar_os_wifi_latency_acquire(lease->owner) : solar_os_wifi_connectionless_acquire(lease->owner, channel, &actual_channel));
    if (!result) { lease->acquired = true; if (actual) *actual = actual_channel; }
    else if (!solar_os_native_handle_close_kind(*out, SOLAR_OS_NATIVE_HANDLE_WIFI_LEASE)) *out = 0;
    return result;
}
static int wifi_connectionless_acquire(const char *owner, uint32_t channel, uint32_t *actual, solar_os_native_handle_t *out)
{ return wifi_acquire(owner, channel, actual, false, out); }
static int wifi_latency_acquire(const char *owner, solar_os_native_handle_t *out) { return wifi_acquire(owner, 0, NULL, true, out); }
static int wifi_lease_close(solar_os_native_handle_t id) { return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_WIFI_LEASE); }
#define WIFI_STATES(X) \
    X(OFF) X(IDLE) X(SCANNING) X(CONNECTING) X(CONNECTED) X(DISCONNECTED) X(FAILED)
static int wifi_status(solar_os_native_wifi_status_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_wifi_status_t value = {0}; solar_os_wifi_get_status(&value);
    switch (value.state) {
#define STATE_OUT(name) case SOLAR_OS_WIFI_STATE_##name: out->state = SOLAR_OS_NATIVE_WIFI_##name; break;
    WIFI_STATES(STATE_OUT)
#undef STATE_OUT
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
#define FLAG(field, bit) if (value.field) out->flags |= SOLAR_OS_NATIVE_WIFI_##bit
    FLAG(initialized, INITIALIZED); FLAG(started, STARTED); FLAG(connected, LINK); FLAG(has_ip, HAS_IP);
    FLAG(has_saved_config, SAVED); FLAG(has_saved_ap_config, AP_SAVED); FLAG(nat_enabled, NAT_ENABLED); FLAG(nat_active, NAT_ACTIVE);
    FLAG(repeater_enabled, REPEATER_ENABLED); FLAG(repeater_active, REPEATER_ACTIVE); FLAG(ap_enabled, AP_ENABLED); FLAG(ap_running, AP_RUNNING);
    FLAG(connectionless_active, CONNECTIONLESS); FLAG(connectionless_channel_auto, AUTO_CHANNEL);
#undef FLAG
#define SCALAR(field) out->field = value.field
    SCALAR(repeater_upstream_frames); SCALAR(repeater_downstream_frames); SCALAR(repeater_dropped_frames);
    SCALAR(channel); SCALAR(disconnect_reason); SCALAR(ap_channel); SCALAR(ap_station_count); SCALAR(ap_max_connections);
    SCALAR(connectionless_channel); SCALAR(saved_profile_count); SCALAR(repeater_learned_clients); SCALAR(rssi);
#undef SCALAR
    out->nat_error = solar_os_native_result(value.nat_last_error);
    COPY_FIELD(out, &value, ssid); COPY_FIELD(out, &value, saved_ssid); COPY_FIELD(out, &value, saved_ap_ssid); COPY_FIELD(out, &value, saved_ap_auth);
    COPY_FIELD(out, &value, ip); COPY_FIELD(out, &value, gateway); COPY_FIELD(out, &value, netmask);
    COPY_FIELD(out, &value, ap_ssid); COPY_FIELD(out, &value, ap_auth); COPY_FIELD(out, &value, ap_ip); COPY_FIELD(out, &value, connectionless_owner);
    return SOLAR_OS_NATIVE_OK;
}
static int wifi_status_text(char *out, size_t capacity)
{
    if (!out || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0; solar_os_wifi_get_status_text(out, capacity); return SOLAR_OS_NATIVE_OK;
}
static const char *wifi_state_name(uint32_t state)
{
    switch (state) {
#define STATE_NAME(name) case SOLAR_OS_NATIVE_WIFI_##name: return solar_os_wifi_state_name(SOLAR_OS_WIFI_STATE_##name);
    WIFI_STATES(STATE_NAME)
#undef STATE_NAME
    default: return "unknown";
    }
}

#undef WIFI_STATES
static const solar_os_native_wifi_api_v1_t wifi_api = {
    .abi_version = SOLAR_OS_NATIVE_WIFI_ABI, .struct_size = sizeof(wifi_api), .boot_enabled = wifi_boot_enabled, .boot_enable = wifi_boot_enable,
    .start = wifi_start, .stop = wifi_stop, .connect = wifi_connect, .connect_saved = wifi_connect_saved, .disconnect = wifi_disconnect,
    .forget = wifi_forget, .forget_all = wifi_forget_all, .known = wifi_known, .is_known = solar_os_wifi_is_known_ssid,
    .ap_start = wifi_ap_start, .ap_stop = wifi_ap_stop, .ap_saved = wifi_ap_saved, .ap_save = wifi_ap_save, .ap_forget = wifi_ap_forget,
    .nat_set = wifi_nat_set, .repeater_start = wifi_repeater_start, .repeater_stop = wifi_repeater_stop,
    .scan = wifi_scan, .scan_start = wifi_scan_start, .scan_results = wifi_scan_results, .scan_cancel = wifi_scan_cancel,
    .connectionless_acquire = wifi_connectionless_acquire, .latency_acquire = wifi_latency_acquire, .lease_close = wifi_lease_close,
    .status = wifi_status, .status_text = wifi_status_text, .state_name = wifi_state_name,
};
#endif
const void *solar_os_native_routing_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_routing_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
