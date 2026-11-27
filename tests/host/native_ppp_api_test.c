#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_memory.h"
#include "solar_os_native_ppp.h"
#include "solar_os_native_runtime.h"
#include "solar_os_network.h"
#include "solar_os_ppp.h"
#include <freertos/FreeRTOS.h>

struct esp_netif_obj { int marker; };
struct solar_os_ppp {
    solar_os_ppp_transport_t transport;
    solar_os_ppp_netif_binding_t binding;
    esp_netif_t *netif;
    solar_os_ppp_status_t status;
    bool session_active, link_open, transitioning;
    size_t io_refs;
};
static esp_netif_t netif;
static solar_os_ppp_t *core;
static portMUX_TYPE ppp_io_lock = portMUX_INITIALIZER_UNLOCKED;
static const solar_os_native_ppp_api_v1_t *api;
static solar_os_native_handle_t token;
static unsigned allocations, pins, starts, stops, deliveries;
static bool registered, connecting, ready, fail_stop;
static atomic_bool held, transmitting;

static const uint8_t bytes[] = {0x7e, 0xff, 0x03, 0x21};

static esp_err_t esp_netif_receive(esp_netif_t *device, void *data, size_t size, void *context)
{
    assert(device == &netif && !context && size == sizeof(bytes) && !memcmp(data, bytes, size));
    ++deliveries;
    return ESP_OK;
}
#include "ppp_io.inc"

size_t strlcpy(char *out, const char *in, size_t capacity)
{
    const size_t length = strlen(in);
    if (capacity) {
        const size_t n = length < capacity - 1U ? length : capacity - 1U;
        memcpy(out, in, n);
        out[n] = 0;
    }
    return length;
}

void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t kind, const char *tag)
{
    assert(kind == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag);
    void *pointer = calloc(count, size);
    if (pointer) ++allocations;
    return pointer;
}
void solar_os_memory_free(void *pointer)
{
    if (pointer) {
        assert(allocations);
        --allocations;
        free(pointer);
    }
}
int solar_os_native_library_retain(solar_os_native_handle_t lease, solar_os_native_handle_t *out)
{
    assert(lease == 14 && !pins);
    ++pins;
    *out = 15;
    return SOLAR_OS_NATIVE_OK;
}
int solar_os_native_library_release(solar_os_native_handle_t lease)
{
    assert(lease == 15 && pins == 1 && !core);
    --pins;
    return SOLAR_OS_NATIVE_OK;
}

esp_err_t solar_os_network_path_register(const char *name, esp_netif_t *device, int priority)
{
    assert(!strcmp(name, "test-peer") && device == &netif && priority == 42 && !registered);
    registered = true;
    return ESP_OK;
}
esp_err_t solar_os_network_path_unregister(esp_netif_t *device)
{
    assert(device == &netif && registered && !ready);
    registered = connecting = false;
    return ESP_OK;
}
esp_err_t solar_os_network_path_set_connecting(esp_netif_t *device, bool value)
{
    assert(device == &netif && registered);
    connecting = value && !ready;
    return ESP_OK;
}
esp_err_t solar_os_network_path_set_ready(esp_netif_t *device, bool value)
{
    assert(device == &netif && registered);
    ready = value;
    connecting = false;
    return ESP_OK;
}

esp_err_t solar_os_ppp_create(const solar_os_ppp_config_t *config, solar_os_ppp_t **out)
{
    static const uint8_t peer[] = {192, 0, 2, 5};
    assert(!strcmp(config->name, "test-peer") && config->link.mode == SOLAR_OS_PPP_MODE_PASSIVE);
    assert(!memcmp(&config->link.peer_address.addr, peer, sizeof(peer)) && pins == 1);
    core = calloc(1, sizeof(*core));
    assert(core);
    core->transport = config->transport;
    core->binding = config->netif;
    core->netif = &netif;
    *out = core;
    return ESP_OK;
}
esp_err_t solar_os_ppp_destroy(solar_os_ppp_t *session)
{
    assert(session == core && !session->session_active && !session->link_open && !ppp_io_busy(session));
    if (registered) session->binding.detach(session->binding.ctx, session->netif);
    free(session);
    core = NULL;
    return ESP_OK;
}
esp_err_t solar_os_ppp_start(solar_os_ppp_t *session, const solar_os_ppp_profile_t *profile)
{
    assert(session == core && profile->auth == SOLAR_OS_PPP_AUTH_PAP);
    assert(!strcmp(profile->username, "test-user") && !strcmp(profile->password, "test-password"));
    if (!registered) assert(!session->binding.attach(session->binding.ctx, session->netif));
    assert(connecting && !ready);
    if (!session->transport.start(session->transport.ctx)) {
        session->link_open = true;
        assert(ppp_set_session_locked(session, true));
        session->status.state = SOLAR_OS_PPP_STATE_CONNECTING;
        return ESP_OK;
    }
    return ESP_FAIL;
}
static void *receive_from_transport(void *arg)
{
    (void)arg;
    assert(!api->receive(token, bytes, sizeof(bytes)));
    return NULL;
}
esp_err_t solar_os_ppp_connect(solar_os_ppp_t *session, const solar_os_ppp_profile_t *profile,
    uint32_t timeout)
{
    assert(timeout == 100 && !solar_os_ppp_start(session, profile));
    pthread_t producer;
    assert(!pthread_create(&producer, NULL, receive_from_transport, NULL));
    assert(!pthread_join(producer, NULL));
    session->status.state = SOLAR_OS_PPP_STATE_UP;
    strcpy(session->status.ipv4_address, "192.0.2.6");
    session->binding.set_ready(session->binding.ctx, session->netif, true);
    return ESP_OK;
}
esp_err_t solar_os_ppp_disconnect(solar_os_ppp_t *session)
{
    if (registered) session->binding.set_ready(session->binding.ctx, session->netif, false);
    if (!ppp_set_session_locked(session, false)) return ESP_ERR_NOT_FINISHED;
    if (session->link_open) {
        const esp_err_t error = session->transport.stop(session->transport.ctx);
        if (error != ESP_OK) return error;
        session->link_open = false;
    }
    session->status.state = SOLAR_OS_PPP_STATE_DOWN;
    return ESP_OK;
}
esp_err_t solar_os_ppp_notify_transport_reset(solar_os_ppp_t *session)
{
    assert(ppp_set_session_locked(session, false));
    session->link_open = false;
    session->status = (solar_os_ppp_status_t){0};
    session->binding.set_ready(session->binding.ctx, session->netif, false);
    return ESP_OK;
}
esp_err_t solar_os_ppp_snapshot(solar_os_ppp_t *session, solar_os_ppp_status_t *status,
    bool *busy, bool *connected)
{
    *status = session->status;
    if (busy) *busy = ppp_busy_locked(session);
    if (connected) *connected = ppp_connected_locked(session);
    return ESP_OK;
}
esp_err_t solar_os_ppp_get_status(solar_os_ppp_t *session, solar_os_ppp_status_t *status)
{
    return solar_os_ppp_snapshot(session, status, NULL, NULL);
}

static int open_transport(void *user)
{
    assert(user == &token && pins == 1);
    ++starts;
    solar_os_native_ppp_status_v1_t status;
    assert(api->status(token, &status) == SOLAR_OS_NATIVE_ERROR_BUSY);
    return SOLAR_OS_NATIVE_OK;
}
static int close_transport(void *user)
{
    assert(user == &token && pins == 1);
    ++stops;
    return fail_stop ? SOLAR_OS_NATIVE_ERROR_TIMEOUT : SOLAR_OS_NATIVE_OK;
}
static int write_transport(void *user, const uint8_t *data, size_t size, size_t *written)
{
    assert(user == &token && pins == 1 && size == sizeof(bytes) && !memcmp(data, bytes, size));
    assert(api->close(token) == SOLAR_OS_NATIVE_ERROR_BUSY);
    atomic_store(&transmitting, true);
    while (atomic_load(&held)) sched_yield();
    *written = size;
    return SOLAR_OS_NATIVE_OK;
}
static void *write_from_stack(void *arg)
{
    uint8_t packet[sizeof(bytes)];
    memcpy(packet, bytes, sizeof(packet));
    assert(ppp_transmit(arg, packet, sizeof(packet)) == ESP_OK);
    return NULL;
}

int main(void)
{
    api = solar_os_native_ppp_get_service(SOLAR_OS_NATIVE_PPP_SERVICE, 1, sizeof(*api));
    assert(api);
    solar_os_native_ppp_options_v1_t options = {
        .name = "test-peer", .mode = SOLAR_OS_NATIVE_PPP_PASSIVE, .publish_uplink = true, .route_priority = 42,
        .peer_ipv4 = {192, 0, 2, 5},
        .transport = {.user = &token, .start = open_transport, .stop = close_transport, .write = write_transport},
    };
    const solar_os_native_ppp_profile_v1_t profile = {
        .auth = SOLAR_OS_NATIVE_PPP_AUTH_PAP, .username = "test-user", .password = "test-password",
    };
    assert(api->open(&options, &token) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !token && !allocations);
    options.library_lease = 14;
    assert(!api->open(&options, &token) && pins && token);
    assert(!api->connect(token, &profile, 100) && deliveries == 1 && ready && starts == 1);
    solar_os_native_ppp_status_v1_t status;
    assert(!api->status(token, &status) && status.busy && status.connected && status.state == SOLAR_OS_NATIVE_PPP_UP);
    assert(!strcmp(status.ipv4_address, "192.0.2.6"));
    assert(api->receive(token, bytes, 0) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    atomic_store(&held, true);
    pthread_t stack;
    assert(!pthread_create(&stack, NULL, write_from_stack, core));
    while (!atomic_load(&transmitting)) sched_yield();
    assert(api->close(token) == SOLAR_OS_NATIVE_ERROR_BUSY && core && pins && !stops);
    assert(api->receive(token, bytes, sizeof(bytes)) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    atomic_store(&held, false);
    assert(!pthread_join(stack, NULL));
    fail_stop = true;
    assert(api->close(token) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && core && pins && stops == 1);
    fail_stop = false;
    assert(!api->transport_reset(token) && !ready && stops == 1);
    assert(!api->start(token, &profile) && starts == 2 && connecting);
    assert(!api->close(token) && stops == 2 && !core && !pins && !allocations && !registered);
    assert(api->close(token) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    return 0;
}
