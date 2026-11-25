#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_client.h"
#include "solar_os_native_routing.h"
#include "solar_os_native_handles.h"
#include "solar_os_network.h"
#include "solar_os_wifi.h"
#include "solar_os_memory.h"
static unsigned allocations, claims, releases, release_attempts;
static bool release_fail, allocation_fail;
static char radio_keys[2][32], latency_keys[2][32];
static unsigned radios, latencies;
typedef int wifi_ps_type_t;
static bool wifi_started = true, wifi_connectionless_active, wifi_connectionless_channel_auto;
static uint8_t wifi_connectionless_channel;
static char wifi_connectionless_owner[32], wifi_latency_owners[SOLAR_OS_WIFI_LATENCY_OWNERS_MAX][SOLAR_OS_WIFI_LATENCY_OWNER_MAX];
static unsigned wifi_latency_count;
static void wifi_lock(void) {}
static void wifi_unlock(void) {}
static wifi_ps_type_t wifi_power_save_mode_locked(void) { return wifi_latency_count ? 0 : 1; }
static esp_err_t wifi_apply_mode(void)
{ ++release_attempts; if (release_fail) return ESP_ERR_TIMEOUT; ++releases; return ESP_OK; }
static esp_err_t esp_wifi_set_ps(wifi_ps_type_t mode) { assert(mode == 0 || mode == 1); return wifi_apply_mode(); }
#define SOLAR_OS_LOGI(...) ((void)0)
#include "wifi_release.inc"
#undef SOLAR_OS_LOGI
static const solar_os_native_wifi_api_v1_t *wifi;
static const solar_os_native_routing_api_v1_t *routing;
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag);
    if (allocation_fail) return NULL;
    void *p = calloc(n, size); if (p) ++allocations; return p;
}
void solar_os_memory_free(void *p) { if (p) --allocations; free(p); }
size_t solar_os_network_path_list(solar_os_network_path_info_t *out, size_t capacity)
{
    if (capacity) {
        *out = (solar_os_network_path_info_t){.netif = (void *)1, .route_priority = 255, .ready = true};
        strcpy(out->name, "wifi"); out->dns.ip.type = ESP_IPADDR_TYPE_V6; out->dns.ip.u_addr.ip6.zone = 3;
        for (unsigned i = 0; i < 16; ++i) ((uint8_t *)out->dns.ip.u_addr.ip6.addr)[i] = i;
    }
    return 1;
}
bool solar_os_network_path_get_preferred(solar_os_network_path_info_t *out) { solar_os_network_path_list(out, 1); return true; }
esp_err_t solar_os_network_path_set_priority(const char *name, int priority)
{ assert(!strcmp(name, "wifi") && priority == 17); return ESP_OK; }
size_t solar_os_network_interface_list(solar_os_network_interface_info_t *out, size_t capacity)
{
    if (capacity) {
        *out = (solar_os_network_interface_info_t){.netif = (void *)2, .role = SOLAR_OS_NETWORK_INTERFACE_ROLE_TUNNEL,
            .state = SOLAR_OS_NETWORK_INTERFACE_STATE_ERROR, .last_error = ESP_ERR_TIMEOUT, .nat_enabled = true};
        strcpy(out->name, "tunnel"); strcpy(out->peer, "2001:db8::1");
    }
    return 1;
}
esp_err_t solar_os_network_interface_publish(const solar_os_network_interface_info_t *in)
{
    assert(!in->netif && in->role == SOLAR_OS_NETWORK_INTERFACE_ROLE_TUNNEL);
    assert(in->state == SOLAR_OS_NETWORK_INTERFACE_STATE_ERROR && in->last_error == ESP_ERR_TIMEOUT);
    assert(!strcmp(in->peer, "2001:db8::1")); return ESP_OK;
}
esp_err_t solar_os_network_interface_remove(const char *name) { assert(!strcmp(name, "tunnel")); return ESP_OK; }
void solar_os_network_router_get_status(solar_os_network_router_status_t *out)
{ *out = (solar_os_network_router_status_t){.available = true, .active = true, .client_count = 65535}; strcpy(out->label, "router"); }
esp_err_t solar_os_network_router_start(void) { return ESP_OK; }
esp_err_t solar_os_network_router_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
bool solar_os_wifi_enabled_for_current_boot(void) { return false; }
bool solar_os_wifi_enabled_for_next_boot(void) { return true; }
esp_err_t solar_os_wifi_set_enabled_for_next_boot(bool enabled) { return enabled ? ESP_OK : ESP_ERR_INVALID_STATE; }
#define NOARG(name) esp_err_t solar_os_wifi_##name(void) { return ESP_OK; }
NOARG(start) NOARG(stop) NOARG(connect_saved) NOARG(disconnect) NOARG(forget) NOARG(forget_all)
NOARG(ap_stop) NOARG(ap_forget) NOARG(repeater_start) NOARG(repeater_stop) NOARG(scan_start_async) NOARG(scan_cancel_async)
#undef NOARG
esp_err_t solar_os_wifi_connect(const char *ssid, const char *password)
{ assert(!strcmp(ssid, "peer") && !strcmp(password, "secret")); return ESP_ERR_TIMEOUT; }
esp_err_t solar_os_wifi_forget_ssid(const char *ssid) { assert(!strcmp(ssid, "peer")); return ESP_OK; }
esp_err_t solar_os_wifi_known(solar_os_wifi_profile_t *out, size_t capacity, size_t *count)
{ *count = 1; if (capacity) { memset(out, 0, sizeof(*out)); strcpy(out->ssid, "peer"); out->preferred = true; } return ESP_OK; }
bool solar_os_wifi_is_known_ssid(const char *ssid) { return !strcmp(ssid, "peer"); }
esp_err_t solar_os_wifi_ap_start(const char *ssid, const char *password, const char *auth)
{ assert(!strcmp(ssid, "peer") && !strcmp(password, "secret") && !strcmp(auth, "wpa2")); return ESP_OK; }
esp_err_t solar_os_wifi_ap_save(const char *ssid, const char *password, const char *auth)
{ return solar_os_wifi_ap_start(ssid, password, auth); }
esp_err_t solar_os_wifi_ap_saved_get(solar_os_wifi_ap_config_t *out)
{ memset(out, 0, sizeof(*out)); strcpy(out->ssid, "peer"); strcpy(out->password, "secret"); strcpy(out->auth, "wpa2"); return ESP_OK; }
esp_err_t solar_os_wifi_nat_set(bool enabled) { return enabled ? ESP_OK : ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_wifi_scan(solar_os_wifi_ap_t *out, size_t capacity, size_t *count)
{
    *count = 1;
    if (capacity) { *out = (solar_os_wifi_ap_t){.rssi = -127, .channel = 255, .hidden = true}; strcpy(out->ssid, "peer"); strcpy(out->auth, "wpa2"); }
    return ESP_OK;
}
esp_err_t solar_os_wifi_scan_results(solar_os_wifi_ap_t *out, size_t capacity, size_t *count)
{ return solar_os_wifi_scan(out, capacity, count); }
esp_err_t solar_os_wifi_connectionless_acquire(const char *owner, uint8_t channel, uint8_t *actual)
{
    assert(strstr(owner, "/radio") && !channel);
    if (wifi_connectionless_active) return ESP_ERR_INVALID_STATE;
    assert(radios < 2); strcpy(radio_keys[radios++], owner); ++claims; *actual = 11;
    strcpy(wifi_connectionless_owner, owner); wifi_connectionless_active = true; wifi_connectionless_channel_auto = true; wifi_connectionless_channel = 11;
    return ESP_OK;
}
esp_err_t solar_os_wifi_latency_acquire(const char *owner)
{
    assert(strstr(owner, "/latency") && latencies < 2); strcpy(latency_keys[latencies++], owner); ++claims;
    for (unsigned i = 0; i < SOLAR_OS_WIFI_LATENCY_OWNERS_MAX; ++i) if (!wifi_latency_owners[i][0]) {
        strcpy(wifi_latency_owners[i], owner); ++wifi_latency_count; return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}
void solar_os_wifi_get_status(solar_os_wifi_status_t *out)
{
    *out = (solar_os_wifi_status_t){.state = SOLAR_OS_WIFI_STATE_CONNECTED, .connected = true, .has_ip = true,
        .nat_active = true, .connectionless_channel_auto = true, .rssi = -120, .disconnect_reason = 255,
        .repeater_upstream_frames = UINT64_C(5000000000), .nat_last_error = ESP_ERR_NO_MEM};
    strcpy(out->ssid, "peer"); strcpy(out->connectionless_owner, "radio");
}
void solar_os_wifi_get_status_text(char *out, size_t capacity) { snprintf(out, capacity, "connected"); }
int main(void)
{
    wifi = solar_os_native_routing_get_service("wifi", 1, sizeof(*wifi)); assert(wifi);
    routing = solar_os_native_routing_get_service("network.routing", 1, sizeof(*routing)); assert(routing);
    assert(!solar_os_native_routing_get_service("wifi", 2, 8));
    solar_os_native_network_path_v1_t path; size_t count;
    assert(!routing->paths(NULL, 0, &count) && count == 1);
    assert(!routing->preferred(&path) && !strcmp(path.name, "wifi") && path.priority == 255 && path.dns_family == 6 && path.dns_zone == 3);
    for (unsigned i = 0; i < 16; ++i) assert(path.dns_address[i] == i);
    solar_os_native_network_interface_v1_t interface_info;
    assert(!routing->interfaces(&interface_info, 1, &count) && count == 1);
    assert(interface_info.role == SOLAR_OS_NATIVE_ROUTING_TUNNEL && interface_info.state == SOLAR_OS_NATIVE_ROUTING_ERROR);
    assert(interface_info.error == SOLAR_OS_NATIVE_ERROR_TIMEOUT && interface_info.nat_enabled);
    assert(!routing->interface_publish(&interface_info));
    interface_info.role = SOLAR_OS_NATIVE_ROUTING_UPLINK; assert(routing->interface_publish(&interface_info) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    allocation_fail = true; assert(routing->interfaces(&interface_info, 1, &count) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY); allocation_fail = false;
    solar_os_native_network_router_v1_t router; assert(!routing->router_status(&router) && router.client_count == 65535 && !strcmp(router.label, "router"));
    assert(!routing->priority_set("wifi", 17) && routing->router_stop() == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(!wifi->boot_enabled(false) && wifi->boot_enabled(true)); assert(!wifi->boot_enable(true));
    assert(wifi->connect("peer", "secret") == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    solar_os_native_wifi_ap_v1_t ap; assert(!wifi->scan(&ap, 1, &count) && count == 1 && ap.rssi == -127 && ap.channel == 255 && ap.hidden);
    solar_os_native_wifi_profile_v1_t profile; assert(!wifi->known(&profile, 1, &count) && profile.preferred && !strcmp(profile.ssid, "peer"));
    solar_os_native_wifi_status_v1_t status; assert(!wifi->status(&status));
    assert(status.state == SOLAR_OS_NATIVE_WIFI_CONNECTED && status.rssi == -120 && status.disconnect_reason == 255);
    assert(status.repeater_upstream_frames == UINT64_C(5000000000) && status.nat_error == SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
    assert(status.flags == (SOLAR_OS_NATIVE_WIFI_LINK | SOLAR_OS_NATIVE_WIFI_HAS_IP | SOLAR_OS_NATIVE_WIFI_NAT_ACTIVE | SOLAR_OS_NATIVE_WIFI_AUTO_CHANNEL));
    solar_os_native_handle_t lease, other_lease, latency, latency_other; uint32_t channel;
    assert(wifi->connectionless_acquire("radio", 256, &channel, &lease) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !claims && !lease);
    char owner[] = "radio"; assert(!wifi->connectionless_acquire(owner, 0, &channel, &lease) && channel == 11);
    memset(owner, '?', 5); assert(!wifi->latency_acquire("latency", &latency));
    assert(!wifi->latency_acquire("latency", &latency_other) && strcmp(latency_keys[0], latency_keys[1]));
    assert(wifi->connectionless_acquire("radio", 0, &channel, &other_lease) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !other_lease);
    release_fail = true; assert(wifi->lease_close(lease) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && !releases);
    assert(!wifi_connectionless_active && release_attempts == 1);
    release_fail = false; assert(!wifi->connectionless_acquire("radio", 0, &channel, &other_lease));
    release_fail = false; assert(!wifi->lease_close(lease)); assert(wifi->lease_close(lease) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(wifi_connectionless_active && !strcmp(wifi_connectionless_owner, radio_keys[1]) && release_attempts == 1);
    release_fail = true; assert(wifi->lease_close(latency) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && wifi_latency_count == 1);
    release_fail = false; assert(!wifi->lease_close(latency) && wifi_latency_count == 1 && release_attempts == 2);
    assert(!wifi->lease_close(latency_other) && !wifi->lease_close(other_lease));
    assert(claims == 4 && releases == 2 && release_attempts == 4 && !wifi_latency_count && !wifi_connectionless_active);
    assert(!allocations && !solar_os_native_handle_count()); puts("native routing, DNS, WiFi values and radio ownership passed"); return 0;
}
