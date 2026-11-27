#include "solar_os_native_ppp.h"
#include "solar_os_config.h"

#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_PPP
#include <stdatomic.h>
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_runtime.h"
#include "solar_os_native_support.h"
#include "solar_os_network.h"
#include "solar_os_ppp.h"

_Static_assert(SOLAR_OS_NATIVE_PPP_AUTH_NONE == SOLAR_OS_PPP_AUTH_NONE &&
    SOLAR_OS_NATIVE_PPP_AUTH_PAP == SOLAR_OS_PPP_AUTH_PAP &&
    SOLAR_OS_NATIVE_PPP_AUTH_CHAP == SOLAR_OS_PPP_AUTH_CHAP &&
    SOLAR_OS_NATIVE_PPP_ACTIVE == SOLAR_OS_PPP_MODE_ACTIVE &&
    SOLAR_OS_NATIVE_PPP_PASSIVE == SOLAR_OS_PPP_MODE_PASSIVE &&
    SOLAR_OS_NATIVE_PPP_DOWN == SOLAR_OS_PPP_STATE_DOWN &&
    SOLAR_OS_NATIVE_PPP_CONNECTING == SOLAR_OS_PPP_STATE_CONNECTING &&
    SOLAR_OS_NATIVE_PPP_UP == SOLAR_OS_PPP_STATE_UP &&
    SOLAR_OS_NATIVE_PPP_FAILED == SOLAR_OS_PPP_STATE_FAILED, "PPP ABI values");

typedef struct {
    char name[SOLAR_OS_PPP_NAME_MAX + 1U];
    solar_os_ppp_t *session;
    esp_netif_t *route_netif;
    solar_os_native_ppp_transport_v1_t transport;
    int route_priority;
    solar_os_native_handle_t lease;
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    void *driver_pin;
#endif
    atomic_bool operation;
} native_ppp_t;

static _Thread_local native_ppp_t *transport_scope;

static int native_result(esp_err_t error)
{
    return error == ESP_ERR_NOT_FINISHED ? SOLAR_OS_NATIVE_ERROR_BUSY : solar_os_native_result(error);
}

static esp_err_t transport_control(void *opaque, bool start)
{
    native_ppp_t *state = opaque;
    native_ppp_t *previous = transport_scope;
    transport_scope = state;
    const int result = (start ? state->transport.start : state->transport.stop)(state->transport.user);
    transport_scope = previous;
    return solar_os_native_callback_result(result);
}

static esp_err_t transport_start(void *state) { return transport_control(state, true); }
static esp_err_t transport_stop(void *state) { return transport_control(state, false); }

static esp_err_t transport_read(void *opaque, uint8_t *data, size_t capacity,
    uint32_t timeout, size_t *received)
{
    native_ppp_t *state = opaque;
    native_ppp_t *previous = transport_scope;
    transport_scope = state;
    const int result = state->transport.read(state->transport.user, data, capacity, timeout, received);
    transport_scope = previous;
    return solar_os_native_callback_result(result);
}

static esp_err_t transport_write(void *opaque, const uint8_t *data, size_t size, size_t *written)
{
    native_ppp_t *state = opaque;
    native_ppp_t *previous = transport_scope;
    transport_scope = state;
    const int result = state->transport.write(state->transport.user, data, size, written);
    transport_scope = previous;
    return solar_os_native_callback_result(result);
}

static esp_err_t route_attach(void *opaque, esp_netif_t *netif)
{
    native_ppp_t *state = opaque;
    const esp_err_t error = solar_os_network_path_register(state->name, netif, state->route_priority);
    if (error == ESP_OK) {
        state->route_netif = netif;
        (void)solar_os_network_path_set_connecting(netif, true);
    }
    return error;
}

static void route_ready(void *opaque, esp_netif_t *netif, bool ready)
{
    (void)opaque;
    (void)solar_os_network_path_set_ready(netif, ready);
}

static void route_detach(void *opaque, esp_netif_t *netif)
{
    native_ppp_t *state = opaque;
    (void)solar_os_network_path_unregister(netif);
    state->route_netif = NULL;
}

static int ppp_destroy(void *opaque)
{
    native_ppp_t *state = opaque;
    if (transport_scope == state) return SOLAR_OS_NATIVE_ERROR_BUSY;
    if (state->session) {
        esp_err_t error = solar_os_ppp_disconnect(state->session);
        if (!error) error = solar_os_ppp_destroy(state->session);
        if (error != ESP_OK) return native_result(error);
        state->session = NULL;
    }
    if (state->lease) {
        const int result = solar_os_native_library_release(state->lease);
        if (result != SOLAR_OS_NATIVE_OK && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
    }
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (state->driver_pin) solar_os_native_driver_unpin(state->driver_pin);
#endif
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int ppp_close(solar_os_native_handle_t id)
{
    return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_PPP);
}

static int ppp_open(const solar_os_native_ppp_options_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->name || !in->name[0] || strnlen(in->name, SOLAR_OS_PPP_NAME_MAX + 1U) > SOLAR_OS_PPP_NAME_MAX ||
        in->mode > SOLAR_OS_NATIVE_PPP_PASSIVE || !in->transport.write ||
        ((in->transport.start == NULL) != (in->transport.stop == NULL)) ||
        (in->publish_uplink && (in->route_priority < SOLAR_OS_NETWORK_PRIORITY_MIN ||
            in->route_priority > SOLAR_OS_NETWORK_PRIORITY_MAX))) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    native_ppp_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.ppp");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    strlcpy(state->name, in->name, sizeof(state->name));
    state->transport = in->transport;
    state->route_priority = in->route_priority;
    atomic_init(&state->operation, false);
    solar_os_native_handle_t id;
    int result = solar_os_native_handle_create_exclusive(SOLAR_OS_NATIVE_HANDLE_PPP,
        state, ppp_destroy, NULL, &id);
    if (result) {
        solar_os_memory_free(state);
        return result;
    }
    if (in->library_lease) {
        result = solar_os_native_library_retain(in->library_lease, &state->lease);
    } else {
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
        state->driver_pin = solar_os_native_driver_pin();
        result = state->driver_pin ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#else
        result = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#endif
    }

    if (!result) {
        solar_os_ppp_config_t config = {
            .name = state->name, .read_timeout_ms = in->read_timeout_ms,
            .transport = {.ctx = state, .start = in->transport.start ? transport_start : NULL,
                .stop = in->transport.stop ? transport_stop : NULL, .read = in->transport.read ? transport_read : NULL,
                .write = transport_write},
            .link.mode = (solar_os_ppp_mode_t)in->mode,
        };
        if (in->publish_uplink) config.netif = (solar_os_ppp_netif_binding_t){
            .ctx = state, .attach = route_attach, .set_ready = route_ready, .detach = route_detach,
        };
#define COPY_IPV4(target, source) do { \
    _Static_assert(sizeof(config.link.target.addr) == sizeof(in->source), "PPP IPv4 bytes"); \
    memcpy(&config.link.target.addr, in->source, sizeof(in->source)); \
} while (0)
        COPY_IPV4(local_address, local_ipv4);
        COPY_IPV4(peer_address, peer_ipv4);
        COPY_IPV4(primary_dns, primary_dns);
        COPY_IPV4(secondary_dns, secondary_dns);
#undef COPY_IPV4
        result = native_result(solar_os_ppp_create(&config, &state->session));
    }
    solar_os_native_handle_release_exclusive(id);
    if (result && ppp_close(id) == SOLAR_OS_NATIVE_OK) return result;
    *out = id;
    return result;
}

static int acquire(solar_os_native_handle_t id, native_ppp_t **out, bool lifecycle)
{
    native_ppp_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_PPP);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (!state->session || transport_scope == state || (lifecycle && atomic_exchange(&state->operation, true))) {
        const bool exists = state->session != NULL;
        solar_os_native_handle_release(id);
        return exists ? SOLAR_OS_NATIVE_ERROR_BUSY : SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    *out = state;
    return SOLAR_OS_NATIVE_OK;
}

static void release(solar_os_native_handle_t id, native_ppp_t *state, bool lifecycle)
{
    if (lifecycle) atomic_store(&state->operation, false);
    solar_os_native_handle_release(id);
}

static int start_session(solar_os_native_handle_t id, const solar_os_native_ppp_profile_v1_t *in,
    uint32_t timeout)
{
    if (!in || in->auth > SOLAR_OS_NATIVE_PPP_AUTH_CHAP) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_ppp_t *state;
    const int result = acquire(id, &state, true);
    if (result) return result;

    solar_os_ppp_profile_t profile = {.auth = (solar_os_ppp_auth_t)in->auth};
#define COPY(field) do { \
    _Static_assert(sizeof(profile.field) == sizeof(in->field), "PPP profile capacity"); \
    memcpy(profile.field, in->field, sizeof(profile.field)); \
} while (0)
    COPY(username);
    COPY(password);
    COPY(dns);
#undef COPY
    if (state->route_netif) (void)solar_os_network_path_set_connecting(state->route_netif, true);
    const esp_err_t error = timeout ? solar_os_ppp_connect(state->session, &profile, timeout) :
        solar_os_ppp_start(state->session, &profile);
    explicit_bzero(&profile, sizeof(profile));
    if (state->route_netif) {
        solar_os_ppp_status_t status;
        if (!solar_os_ppp_get_status(state->session, &status))
            (void)solar_os_network_path_set_connecting(state->route_netif, status.state == SOLAR_OS_PPP_STATE_CONNECTING);
    }
    release(id, state, true);
    return native_result(error);
}

static int ppp_start(solar_os_native_handle_t id, const solar_os_native_ppp_profile_v1_t *profile)
{
    return start_session(id, profile, 0);
}

static int ppp_connect(solar_os_native_handle_t id, const solar_os_native_ppp_profile_v1_t *profile, uint32_t timeout)
{
    return timeout ? start_session(id, profile, timeout) : SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
}

static int stop_session(solar_os_native_handle_t id, bool reset)
{
    native_ppp_t *state;
    const int result = acquire(id, &state, true);
    if (result) return result;
    const esp_err_t error = reset ? solar_os_ppp_notify_transport_reset(state->session) : solar_os_ppp_disconnect(state->session);
    release(id, state, true);
    return native_result(error);
}

static int ppp_disconnect(solar_os_native_handle_t id) { return stop_session(id, false); }
static int ppp_transport_reset(solar_os_native_handle_t id) { return stop_session(id, true); }

static int ppp_receive(solar_os_native_handle_t id, const uint8_t *data, size_t size)
{
    native_ppp_t *state;
    const int result = acquire(id, &state, false);
    if (result) return result;
    const esp_err_t error = state->transport.read ? ESP_ERR_INVALID_STATE : solar_os_ppp_receive(state->session, data, size);
    release(id, state, false);
    return native_result(error);
}

static int ppp_status(solar_os_native_handle_t id, solar_os_native_ppp_status_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_ppp_status_v1_t){0};
    native_ppp_t *state;
    const int result = acquire(id, &state, false);
    if (result) return result;
    solar_os_ppp_status_t status;
    const esp_err_t error = solar_os_ppp_snapshot(state->session, &status, &out->busy, &out->connected);
    if (!error) {
        out->state = status.state;
        out->error = native_result(status.error);
#define COPY(field) do { \
    _Static_assert(sizeof(out->field) == sizeof(status.field), "PPP status capacity"); \
    memcpy(out->field, status.field, sizeof(out->field)); \
} while (0)
        COPY(interface_name);
        COPY(ipv4_address);
        COPY(ipv4_gateway);
        COPY(dns_address);
#undef COPY
    }
    release(id, state, false);
    return native_result(error);
}

static const solar_os_native_ppp_api_v1_t ppp_api = {
    .abi_version = SOLAR_OS_NATIVE_PPP_ABI, .struct_size = sizeof(ppp_api),
    .open = ppp_open, .start = ppp_start, .connect = ppp_connect, .disconnect = ppp_disconnect,
    .transport_reset = ppp_transport_reset, .receive = ppp_receive, .status = ppp_status, .close = ppp_close,
};
#endif

const void *solar_os_native_ppp_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_ppp_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
