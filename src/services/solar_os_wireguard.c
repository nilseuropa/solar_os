#include "solar_os_wireguard.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/ip.h"
#include "lwip/ip4.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip.h"
#include "lwip/tcpip.h"
#include "nvs.h"
#include "solar_os_log.h"
#include "solar_os_lwip_route.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"
#include "solar_os_time.h"
#include "solar_os_wifi.h"
#include "wireguard.h"
#include "wireguardif.h"
#include "crypto.h"

#define TAG "wireguard"
#define WIREGUARD_NVS_NAMESPACE "wireguard"
#define WIREGUARD_NVS_PROFILE_KEY "profile"
#define WIREGUARD_NVS_TAI_CEILING_KEY "tai_ceiling"
#define WIREGUARD_PROFILE_MAGIC 0x57475031UL
#define WIREGUARD_PROFILE_VERSION 1U
#define WIREGUARD_KEY_TEXT_LEN 45U
#define WIREGUARD_CONFIG_LINE_MAX 255U
#define WIREGUARD_WORKER_STACK 6144U
#define WIREGUARD_WORKER_PRIORITY 5U
#define WIREGUARD_RETRY_MS 5000U
#define WIREGUARD_TAI_RESERVATION_SECONDS 3155760000ULL
#define WIREGUARD_TAI64_EPOCH_OFFSET 0x400000000000000aULL

typedef struct {
    uint32_t network;
    uint8_t prefix;
    uint8_t reserved[3];
} wireguard_route_store_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    char private_key[WIREGUARD_KEY_TEXT_LEN];
    char public_key[WIREGUARD_KEY_TEXT_LEN];
    char preshared_key[WIREGUARD_KEY_TEXT_LEN];
    uint32_t address;
    uint32_t dns;
    uint8_t address_prefix;
    uint8_t dns_valid;
    uint8_t route_count;
    uint8_t reserved0;
    uint16_t listen_port;
    uint16_t endpoint_port;
    uint16_t keepalive_seconds;
    uint16_t mtu;
    char endpoint[SOLAR_OS_WIREGUARD_ENDPOINT_MAX + 1U];
    wireguard_route_store_t routes[SOLAR_OS_WIREGUARD_ROUTE_MAX];
} wireguard_profile_store_t;

typedef struct {
    struct netif *wifi_netif;
    ip4_addr_t endpoint_ip;
    wireguard_profile_store_t profile;
    uint8_t preshared_key[WIREGUARD_SESSION_KEY_LEN];
    bool has_preshared_key;
    solar_os_wireguard_policy_t policy;
    err_t result;
} wireguard_start_request_t;

typedef struct {
    SemaphoreHandle_t mutex;
    TaskHandle_t worker;
    wireguard_profile_store_t *active_profile;
    bool worker_stop_requested;
    volatile bool worker_done;
    bool initialized;
    bool configured;
    bool desired_up;
    bool suspended;
    bool wifi_has_ip;
    bool runtime_active;
    bool peer_up;
    bool full_tunnel;
    bool kill_switch_active;
    bool filter_installed;
    bool dns_overridden;
    bool time_ready;
    solar_os_wireguard_state_t state;
    solar_os_wireguard_policy_t policy;
    esp_err_t last_error;
    uint32_t last_retry_ms;
    uint64_t tai_base_seconds;
    uint64_t tai_started_ms;
    struct netif wg_netif;
    struct netif *wifi_netif;
    netif_output_fn wifi_output;
#if LWIP_IPV6
    netif_output_ip6_fn wifi_output_ip6;
#endif
    u8_t peer_index;
    ip4_addr_t endpoint_ip;
    ip_addr_t saved_dns;
    wireguard_profile_store_t summary;
    char peer_key_fingerprint[17];
} wireguard_service_t;

static wireguard_service_t wireguard_service;
static const struct wireguardif_init_data *wireguard_pending_init_data;

static void wireguard_worker(void *argument);

static void wireguard_lock(void)
{
    if (wireguard_service.mutex != NULL) {
        xSemaphoreTake(wireguard_service.mutex, portMAX_DELAY);
    }
}

static void wireguard_unlock(void)
{
    if (wireguard_service.mutex != NULL) {
        xSemaphoreGive(wireguard_service.mutex);
    }
}

static void wireguard_set_detail(char *detail,
                                 size_t detail_len,
                                 size_t line,
                                 const char *message)
{
    if (detail == NULL || detail_len == 0U) {
        return;
    }
    if (line > 0U) {
        snprintf(detail, detail_len, "line %u: %s", (unsigned)line, message);
    } else {
        strlcpy(detail, message, detail_len);
    }
}

static char *wireguard_trim(char *text)
{
    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return text;
}

static bool wireguard_parse_u16(const char *text,
                                uint16_t min,
                                uint16_t max,
                                uint16_t *value)
{
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }
    *value = (uint16_t)parsed;
    return true;
}

static uint32_t wireguard_prefix_mask(uint8_t prefix)
{
    if (prefix == 0U) {
        return 0U;
    }
    return PP_HTONL(0xffffffffUL << (32U - prefix));
}

static bool wireguard_parse_cidr(const char *text, uint32_t *address, uint8_t *prefix)
{
    char copy[32];
    if (text == NULL || strlen(text) >= sizeof(copy)) {
        return false;
    }
    strlcpy(copy, text, sizeof(copy));
    char *slash = strchr(copy, '/');
    if (slash == NULL) {
        return false;
    }
    *slash++ = '\0';

    uint16_t parsed_prefix = 0;
    ip4_addr_t parsed_address;
    if (!wireguard_parse_u16(slash, 0U, 32U, &parsed_prefix) ||
        ip4addr_aton(copy, &parsed_address) == 0) {
        return false;
    }
    *address = parsed_address.addr;
    *prefix = (uint8_t)parsed_prefix;
    return true;
}

static bool wireguard_key_is_valid(const char *key, bool optional)
{
    uint8_t decoded[WIREGUARD_PRIVATE_KEY_LEN];
    size_t decoded_len = sizeof(decoded);
    bool valid = false;

    if (key == NULL || key[0] == '\0') {
        return optional;
    }
    if (strlen(key) == 44U &&
        wireguard_base64_decode(key, decoded, &decoded_len) &&
        decoded_len == sizeof(decoded)) {
        valid = true;
    }
    crypto_zero(decoded, sizeof(decoded));
    return valid;
}

static bool wireguard_parse_endpoint(const char *text,
                                     char *host,
                                     size_t host_len,
                                     uint16_t *port)
{
    if (text == NULL || text[0] == '[' || strlen(text) >= host_len + 7U) {
        return false;
    }
    const char *colon = strrchr(text, ':');
    if (colon == NULL || colon == text || colon[1] == '\0') {
        return false;
    }
    const size_t length = (size_t)(colon - text);
    if (length >= host_len || !wireguard_parse_u16(colon + 1, 1U, 65535U, port)) {
        return false;
    }
    memcpy(host, text, length);
    host[length] = '\0';
    return true;
}

static esp_err_t wireguard_profile_validate(const wireguard_profile_store_t *profile)
{
    if (profile == NULL || profile->magic != WIREGUARD_PROFILE_MAGIC ||
        profile->version != WIREGUARD_PROFILE_VERSION ||
        profile->size != sizeof(*profile) ||
        !wireguard_key_is_valid(profile->private_key, false) ||
        !wireguard_key_is_valid(profile->public_key, false) ||
        !wireguard_key_is_valid(profile->preshared_key, true) ||
        profile->address_prefix > 32U || profile->address == 0U ||
        profile->endpoint[0] == '\0' || profile->endpoint_port == 0U ||
        profile->route_count == 0U ||
        profile->route_count > SOLAR_OS_WIREGUARD_ROUTE_MAX ||
        profile->mtu < 576U || profile->mtu > 1420U) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (size_t i = 0; i < profile->route_count; i++) {
        if (profile->routes[i].prefix > 32U) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_OK;
}

static esp_err_t wireguard_profile_load(wireguard_profile_store_t *profile)
{
    if (profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(profile, 0, sizeof(*profile));

    nvs_handle_t nvs;
    esp_err_t error = nvs_open(WIREGUARD_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (error != ESP_OK) {
        return error;
    }
    size_t length = sizeof(*profile);
    error = nvs_get_blob(nvs, WIREGUARD_NVS_PROFILE_KEY, profile, &length);
    nvs_close(nvs);
    if (error != ESP_OK) {
        crypto_zero(profile, sizeof(*profile));
        return error;
    }
    if (length != sizeof(*profile)) {
        crypto_zero(profile, sizeof(*profile));
        return ESP_ERR_INVALID_SIZE;
    }
    error = wireguard_profile_validate(profile);
    if (error != ESP_OK) {
        crypto_zero(profile, sizeof(*profile));
    }
    return error;
}

static esp_err_t wireguard_active_profile_prepare(void)
{
    wireguard_lock();
    const bool prepared = wireguard_service.active_profile != NULL;
    wireguard_unlock();
    if (prepared) {
        return ESP_OK;
    }

    wireguard_profile_store_t *profile = solar_os_memory_calloc(
        1U,
        sizeof(*profile),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "wireguard.profile");
    if (profile == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t error = wireguard_profile_load(profile);
    if (error != ESP_OK) {
        crypto_zero(profile, sizeof(*profile));
        solar_os_memory_free(profile);
        return error;
    }

    wireguard_lock();
    if (wireguard_service.active_profile == NULL) {
        wireguard_service.active_profile = profile;
        profile = NULL;
    }
    wireguard_unlock();

    if (profile != NULL) {
        crypto_zero(profile, sizeof(*profile));
        solar_os_memory_free(profile);
    }
    return ESP_OK;
}

static void wireguard_active_profile_clear(void)
{
    wireguard_lock();
    wireguard_profile_store_t *profile = wireguard_service.active_profile;
    wireguard_service.active_profile = NULL;
    wireguard_unlock();

    if (profile != NULL) {
        crypto_zero(profile, sizeof(*profile));
        solar_os_memory_free(profile);
    }
}

static esp_err_t wireguard_profile_save(const wireguard_profile_store_t *profile)
{
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(WIREGUARD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_blob(nvs, WIREGUARD_NVS_PROFILE_KEY, profile, sizeof(*profile));
    if (error == ESP_OK) {
        error = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return error;
}

static esp_err_t wireguard_profile_erase(void)
{
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(WIREGUARD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_erase_key(nvs, WIREGUARD_NVS_PROFILE_KEY);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        error = ESP_OK;
    }
    if (error == ESP_OK) {
        error = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return error;
}

static void wireguard_profile_fingerprint(const wireguard_profile_store_t *profile,
                                          char output[17])
{
    uint8_t key[WIREGUARD_PUBLIC_KEY_LEN];
    size_t length = sizeof(key);
    output[0] = '\0';
    if (wireguard_base64_decode(profile->public_key, key, &length) && length == sizeof(key)) {
        for (size_t i = 0; i < 8U; i++) {
            snprintf(output + i * 2U, 3U, "%02x", key[i]);
        }
    }
    crypto_zero(key, sizeof(key));
}

static bool wireguard_profile_is_full_tunnel(const wireguard_profile_store_t *profile)
{
    for (size_t i = 0; i < profile->route_count; i++) {
        if (profile->routes[i].prefix == 0U) {
            return true;
        }
    }
    return false;
}

static void wireguard_summary_set(const wireguard_profile_store_t *profile)
{
    wireguard_lock();
    memset(&wireguard_service.summary, 0, sizeof(wireguard_service.summary));
    ip4_addr_set_zero(&wireguard_service.endpoint_ip);
    if (profile != NULL) {
        wireguard_service.summary.magic = profile->magic;
        wireguard_service.summary.version = profile->version;
        wireguard_service.summary.size = profile->size;
        wireguard_service.summary.address = profile->address;
        wireguard_service.summary.dns = profile->dns;
        wireguard_service.summary.address_prefix = profile->address_prefix;
        wireguard_service.summary.dns_valid = profile->dns_valid;
        wireguard_service.summary.route_count = profile->route_count;
        wireguard_service.summary.listen_port = profile->listen_port;
        wireguard_service.summary.endpoint_port = profile->endpoint_port;
        wireguard_service.summary.keepalive_seconds = profile->keepalive_seconds;
        wireguard_service.summary.mtu = profile->mtu;
        strlcpy(wireguard_service.summary.endpoint,
                profile->endpoint,
                sizeof(wireguard_service.summary.endpoint));
        memcpy(wireguard_service.summary.routes,
               profile->routes,
               sizeof(wireguard_service.summary.routes));
        wireguard_profile_fingerprint(profile, wireguard_service.peer_key_fingerprint);
        wireguard_service.configured = true;
        wireguard_service.full_tunnel = wireguard_profile_is_full_tunnel(profile);
    } else {
        wireguard_service.peer_key_fingerprint[0] = '\0';
        wireguard_service.configured = false;
        wireguard_service.full_tunnel = false;
    }
    wireguard_unlock();
}

static esp_err_t wireguard_time_reserve(void)
{
    uint64_t wall_ms = 0;
    uint64_t wall_seconds = 0;
    if (solar_os_time_get_utc_epoch_ms(&wall_ms) == ESP_OK) {
        wall_seconds = wall_ms / 1000ULL;
    }

    nvs_handle_t nvs;
    esp_err_t error = nvs_open(WIREGUARD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (error != ESP_OK) {
        return error;
    }
    uint64_t stored_ceiling = 0;
    const esp_err_t read_error = nvs_get_u64(nvs,
                                             WIREGUARD_NVS_TAI_CEILING_KEY,
                                             &stored_ceiling);
    if (read_error != ESP_OK && read_error != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return read_error;
    }

    uint64_t base = wall_seconds;
    if (stored_ceiling > base) {
        base = stored_ceiling;
    }
    if (base == 0U) {
        base = 1U;
    }
    if (UINT64_MAX - base < WIREGUARD_TAI_RESERVATION_SECONDS) {
        nvs_close(nvs);
        return ESP_ERR_INVALID_SIZE;
    }
    const uint64_t new_ceiling = base + WIREGUARD_TAI_RESERVATION_SECONDS;
    error = nvs_set_u64(nvs, WIREGUARD_NVS_TAI_CEILING_KEY, new_ceiling);
    if (error == ESP_OK) {
        error = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (error == ESP_OK) {
        wireguard_service.tai_base_seconds = base;
        wireguard_service.tai_started_ms = solar_os_time_uptime_ms();
    }
    return error;
}

static esp_err_t wireguard_parse_allowed_ips(char *value,
                                             wireguard_profile_store_t *profile)
{
    char *save = NULL;
    for (char *token = strtok_r(value, ",", &save);
         token != NULL;
         token = strtok_r(NULL, ",", &save)) {
        token = wireguard_trim(token);
        if (profile->route_count >= SOLAR_OS_WIREGUARD_ROUTE_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint32_t address = 0;
        uint8_t prefix = 0;
        if (!wireguard_parse_cidr(token, &address, &prefix)) {
            return ESP_ERR_INVALID_ARG;
        }
        const uint32_t mask = wireguard_prefix_mask(prefix);
        wireguard_route_store_t *route = &profile->routes[profile->route_count++];
        route->network = address & mask;
        route->prefix = prefix;
    }
    return profile->route_count > 0U ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t wireguard_parse_config(FILE *file,
                                        wireguard_profile_store_t *profile,
                                        char *detail,
                                        size_t detail_len)
{
    enum { SECTION_NONE, SECTION_INTERFACE, SECTION_PEER } section = SECTION_NONE;
    bool interface_seen = false;
    bool peer_seen = false;
    size_t line_number = 0;
    char line[WIREGUARD_CONFIG_LINE_MAX + 2U];

    memset(profile, 0, sizeof(*profile));
    profile->magic = WIREGUARD_PROFILE_MAGIC;
    profile->version = WIREGUARD_PROFILE_VERSION;
    profile->size = sizeof(*profile);
    profile->listen_port = WIREGUARDIF_DEFAULT_PORT;
    profile->mtu = WIREGUARDIF_MTU;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;
        const size_t raw_len = strlen(line);
        if (raw_len == sizeof(line) - 1U && line[raw_len - 1U] != '\n' && !feof(file)) {
            wireguard_set_detail(detail, detail_len, line_number, "line is too long");
            crypto_zero(line, sizeof(line));
            return ESP_ERR_INVALID_SIZE;
        }

        char *text = wireguard_trim(line);
        if (*text == '\0' || *text == '#' || *text == ';') {
            crypto_zero(line, sizeof(line));
            continue;
        }
        if (*text == '[') {
            if (strcmp(text, "[Interface]") == 0 &&
                !interface_seen && !peer_seen) {
                section = SECTION_INTERFACE;
                interface_seen = true;
            } else if (strcmp(text, "[Peer]") == 0 &&
                       interface_seen && !peer_seen) {
                section = SECTION_PEER;
                peer_seen = true;
            } else {
                wireguard_set_detail(detail, detail_len, line_number,
                                     "expected one [Interface] and one [Peer] section");
                crypto_zero(line, sizeof(line));
                return ESP_ERR_NOT_SUPPORTED;
            }
            crypto_zero(line, sizeof(line));
            continue;
        }

        char *equals = strchr(text, '=');
        if (equals == NULL || section == SECTION_NONE) {
            wireguard_set_detail(detail, detail_len, line_number, "expected key = value");
            crypto_zero(line, sizeof(line));
            return ESP_ERR_INVALID_ARG;
        }
        *equals++ = '\0';
        char *key = wireguard_trim(text);
        char *value = wireguard_trim(equals);
        for (char *comment = value; *comment != '\0'; comment++) {
            if ((*comment == '#' || *comment == ';') &&
                (comment == value || isspace((unsigned char)comment[-1]))) {
                *comment = '\0';
                value = wireguard_trim(value);
                break;
            }
        }

        esp_err_t error = ESP_OK;
        if (section == SECTION_INTERFACE && strcmp(key, "PrivateKey") == 0) {
            if (!wireguard_key_is_valid(value, false)) {
                error = ESP_ERR_INVALID_ARG;
            } else {
                strlcpy(profile->private_key, value, sizeof(profile->private_key));
            }
        } else if (section == SECTION_INTERFACE && strcmp(key, "Address") == 0) {
            if (strchr(value, ',') != NULL ||
                !wireguard_parse_cidr(value, &profile->address, &profile->address_prefix)) {
                error = ESP_ERR_NOT_SUPPORTED;
            }
        } else if (section == SECTION_INTERFACE && strcmp(key, "ListenPort") == 0) {
            if (!wireguard_parse_u16(value, 1U, 65535U, &profile->listen_port)) {
                error = ESP_ERR_INVALID_ARG;
            }
        } else if (section == SECTION_INTERFACE && strcmp(key, "MTU") == 0) {
            if (!wireguard_parse_u16(value, 576U, 1420U, &profile->mtu)) {
                error = ESP_ERR_INVALID_ARG;
            }
        } else if (section == SECTION_INTERFACE && strcmp(key, "DNS") == 0) {
            ip4_addr_t dns;
            if (strchr(value, ',') != NULL || ip4addr_aton(value, &dns) == 0) {
                error = ESP_ERR_NOT_SUPPORTED;
            } else {
                profile->dns = dns.addr;
                profile->dns_valid = 1U;
            }
        } else if (section == SECTION_PEER && strcmp(key, "PublicKey") == 0) {
            if (!wireguard_key_is_valid(value, false)) {
                error = ESP_ERR_INVALID_ARG;
            } else {
                strlcpy(profile->public_key, value, sizeof(profile->public_key));
            }
        } else if (section == SECTION_PEER && strcmp(key, "PresharedKey") == 0) {
            if (!wireguard_key_is_valid(value, false)) {
                error = ESP_ERR_INVALID_ARG;
            } else {
                strlcpy(profile->preshared_key, value, sizeof(profile->preshared_key));
            }
        } else if (section == SECTION_PEER && strcmp(key, "AllowedIPs") == 0) {
            error = wireguard_parse_allowed_ips(value, profile);
        } else if (section == SECTION_PEER && strcmp(key, "Endpoint") == 0) {
            if (!wireguard_parse_endpoint(value,
                                          profile->endpoint,
                                          sizeof(profile->endpoint),
                                          &profile->endpoint_port)) {
                error = ESP_ERR_NOT_SUPPORTED;
            }
        } else if (section == SECTION_PEER && strcmp(key, "PersistentKeepalive") == 0) {
            if (!wireguard_parse_u16(value, 0U, 65535U, &profile->keepalive_seconds)) {
                error = ESP_ERR_INVALID_ARG;
            }
        } else {
            wireguard_set_detail(detail, detail_len, line_number, "unsupported configuration key");
            crypto_zero(line, sizeof(line));
            return ESP_ERR_NOT_SUPPORTED;
        }

        if (error != ESP_OK) {
            wireguard_set_detail(detail,
                                 detail_len,
                                 line_number,
                                 "invalid or unsupported value; secret values are redacted");
            crypto_zero(line, sizeof(line));
            return error;
        }
        crypto_zero(line, sizeof(line));
    }

    crypto_zero(line, sizeof(line));
    const esp_err_t error = wireguard_profile_validate(profile);
    if (error != ESP_OK) {
        wireguard_set_detail(detail, detail_len, 0U,
                             "configuration is incomplete or outside SolarOS IPv4 client limits");
    }
    return error;
}

static esp_err_t wireguard_resolve_endpoint(const char *host, ip4_addr_t *address)
{
    if (ip4addr_aton(host, address) != 0) {
        return ESP_OK;
    }

    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_DGRAM,
    };
    struct addrinfo *result = NULL;
    const int rc = getaddrinfo(host, NULL, &hints, &result);
    if (rc != 0 || result == NULL || result->ai_addr == NULL) {
        if (result != NULL) {
            freeaddrinfo(result);
        }
        return ESP_ERR_NOT_FOUND;
    }
    const struct sockaddr_in *resolved = (const struct sockaddr_in *)result->ai_addr;
    address->addr = resolved->sin_addr.s_addr;
    freeaddrinfo(result);
    return address->addr != 0U ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static bool wireguard_packet_is_allowed(const struct pbuf *packet)
{
    uint8_t header[68];
    if (packet == NULL || packet->tot_len < 20U ||
        pbuf_copy_partial(packet, header, 20U, 0U) != 20U ||
        (header[0] >> 4U) != 4U) {
        return false;
    }
    const size_t ip_header_len = (size_t)(header[0] & 0x0fU) * 4U;
    if (ip_header_len < 20U || ip_header_len + 8U > sizeof(header) ||
        packet->tot_len < ip_header_len + 8U || header[9] != IP_PROTO_UDP ||
        pbuf_copy_partial(packet, header, (u16_t)(ip_header_len + 8U), 0U) !=
            ip_header_len + 8U) {
        return false;
    }

    uint32_t destination = 0;
    memcpy(&destination, &header[16], sizeof(destination));
    const uint16_t source_port = (uint16_t)((header[ip_header_len] << 8U) |
                                             header[ip_header_len + 1U]);
    const uint16_t destination_port = (uint16_t)((header[ip_header_len + 2U] << 8U) |
                                                  header[ip_header_len + 3U]);
    if (destination == wireguard_service.endpoint_ip.addr &&
        destination_port == wireguard_service.summary.endpoint_port) {
        return true;
    }
    if ((source_port == 67U && destination_port == 68U) ||
        (source_port == 68U && destination_port == 67U)) {
        return true;
    }
    return wireguard_service.endpoint_ip.addr == 0U &&
        wireguard_service.state == SOLAR_OS_WIREGUARD_STATE_RESOLVING &&
        destination_port == 53U;
}

static bool wireguard_destination_is_protected(const ip4_addr_t *destination)
{
    if (wireguard_service.full_tunnel) {
        return true;
    }
    if (destination == NULL) {
        return false;
    }
    for (size_t i = 0; i < wireguard_service.summary.route_count; i++) {
        const uint32_t mask =
            wireguard_prefix_mask(wireguard_service.summary.routes[i].prefix);
        if ((destination->addr & mask) ==
            wireguard_service.summary.routes[i].network) {
            return true;
        }
    }
    return false;
}

static err_t wireguard_wifi_output_filter(struct netif *netif,
                                          struct pbuf *packet,
                                          const ip4_addr_t *destination)
{
    if (wireguard_service.wifi_output == NULL) {
        return ERR_IF;
    }
    if (wireguard_packet_is_allowed(packet) ||
        !wireguard_destination_is_protected(destination)) {
        return wireguard_service.wifi_output(netif, packet, destination);
    }
    return ERR_RTE;
}

#if LWIP_IPV6
static err_t wireguard_wifi_output_ip6_filter(struct netif *netif,
                                              struct pbuf *packet,
                                              const ip6_addr_t *destination)
{
    if (!wireguard_service.full_tunnel &&
        wireguard_service.wifi_output_ip6 != NULL) {
        return wireguard_service.wifi_output_ip6(netif, packet, destination);
    }
    return ERR_RTE;
}
#endif

static void wireguard_filter_install(struct netif *wifi_netif)
{
    LWIP_ASSERT_CORE_LOCKED();
    if (wifi_netif == NULL || wireguard_service.filter_installed) {
        return;
    }
    wireguard_service.wifi_output = wifi_netif->output;
    wifi_netif->output = wireguard_wifi_output_filter;
#if LWIP_IPV6
    wireguard_service.wifi_output_ip6 = wifi_netif->output_ip6;
    wifi_netif->output_ip6 = wireguard_wifi_output_ip6_filter;
#endif
    wireguard_service.filter_installed = true;
    wireguard_service.kill_switch_active = true;
}

static void wireguard_filter_restore(void)
{
    LWIP_ASSERT_CORE_LOCKED();
    if (!wireguard_service.filter_installed || wireguard_service.wifi_netif == NULL) {
        wireguard_service.kill_switch_active = false;
        return;
    }
    wireguard_service.wifi_netif->output = wireguard_service.wifi_output;
#if LWIP_IPV6
    wireguard_service.wifi_netif->output_ip6 = wireguard_service.wifi_output_ip6;
#endif
    wireguard_service.wifi_output = NULL;
#if LWIP_IPV6
    wireguard_service.wifi_output_ip6 = NULL;
#endif
    wireguard_service.filter_installed = false;
    wireguard_service.kill_switch_active = false;
}

static void wireguard_filter_install_tcpip(void *argument)
{
    struct netif *wifi_netif = (struct netif *)argument;
    LWIP_ASSERT_CORE_LOCKED();
    if (wifi_netif != NULL) {
        wireguard_service.wifi_netif = wifi_netif;
        wireguard_filter_install(wifi_netif);
    }
}

static esp_err_t wireguard_enforce_fail_closed(void)
{
    esp_netif_t *wifi = solar_os_wifi_get_sta_netif();
    struct netif *wifi_netif = wifi != NULL ? esp_netif_get_netif_impl(wifi) : NULL;
    if (wifi_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return tcpip_callback_wait(wireguard_filter_install_tcpip,
                               wifi_netif) == ERR_OK ? ESP_OK : ESP_FAIL;
}

static void wireguard_dns_apply(const wireguard_profile_store_t *profile)
{
    LWIP_ASSERT_CORE_LOCKED();
    if (profile->dns_valid == 0U || wireguard_service.dns_overridden) {
        return;
    }
    const ip_addr_t *current = dns_getserver(0U);
    if (current != NULL) {
        wireguard_service.saved_dns = *current;
    } else {
        ip_addr_set_zero(&wireguard_service.saved_dns);
    }
    ip_addr_t dns;
    ip_addr_set_ip4_u32(&dns, profile->dns);
    dns_setserver(0U, &dns);
    wireguard_service.dns_overridden = true;
}

static void wireguard_dns_restore(void)
{
    LWIP_ASSERT_CORE_LOCKED();
    if (wireguard_service.dns_overridden) {
        dns_setserver(0U, &wireguard_service.saved_dns);
        wireguard_service.dns_overridden = false;
    }
}

static void wireguard_apply_link_policy(bool peer_up)
{
    LWIP_ASSERT_CORE_LOCKED();
    if (!wireguard_service.runtime_active) {
        return;
    }

    if (wireguard_service.policy == SOLAR_OS_WIREGUARD_POLICY_FAIL_CLOSED) {
        wireguard_filter_install(wireguard_service.wifi_netif);
    } else {
        wireguard_filter_restore();
    }
    if (!wireguard_service.full_tunnel) {
        return;
    }

    if (wireguard_service.policy == SOLAR_OS_WIREGUARD_POLICY_FAIL_CLOSED) {
        netif_set_default(&wireguard_service.wg_netif);
        wireguard_dns_apply(&wireguard_service.summary);
    } else if (peer_up) {
        wireguard_filter_restore();
        netif_set_default(&wireguard_service.wg_netif);
        wireguard_dns_apply(&wireguard_service.summary);
    } else {
        wireguard_filter_restore();
        if (wireguard_service.wifi_netif != NULL) {
            netif_set_default(wireguard_service.wifi_netif);
        }
        wireguard_dns_restore();
    }
}

static void wireguard_routes_configure(const wireguard_profile_store_t *profile)
{
    solar_os_lwip_route_t routes[SOLAR_OS_LWIP_ROUTE_MAX];
    memset(routes, 0, sizeof(routes));
    for (size_t i = 0; i < profile->route_count; i++) {
        routes[i].network.addr = profile->routes[i].network;
        routes[i].netmask.addr = wireguard_prefix_mask(profile->routes[i].prefix);
    }
    solar_os_lwip_route_configure(&wireguard_service.wg_netif,
                                  routes,
                                  profile->route_count,
                                  wireguard_service.policy ==
                                      SOLAR_OS_WIREGUARD_POLICY_FAIL_CLOSED);
}

static void wireguard_stop_tcpip(void *argument)
{
    const bool preserve_fail_closed = argument != NULL;
    LWIP_ASSERT_CORE_LOCKED();

    if (wireguard_service.runtime_active) {
        solar_os_lwip_route_clear(&wireguard_service.wg_netif);
        (void)wireguardif_disconnect(&wireguard_service.wg_netif,
                                     wireguard_service.peer_index);
        (void)wireguardif_shutdown(&wireguard_service.wg_netif);
        netif_remove(&wireguard_service.wg_netif);
        memset(&wireguard_service.wg_netif, 0, sizeof(wireguard_service.wg_netif));
        wireguard_service.runtime_active = false;
        wireguard_service.peer_up = false;
        wireguard_service.peer_index = WIREGUARDIF_INVALID_INDEX;
    }

    if (wireguard_service.wifi_netif != NULL) {
        netif_set_default(wireguard_service.wifi_netif);
    }
    if (preserve_fail_closed &&
        wireguard_service.policy == SOLAR_OS_WIREGUARD_POLICY_FAIL_CLOSED) {
        wireguard_filter_install(wireguard_service.wifi_netif);
    } else {
        wireguard_filter_restore();
        wireguard_dns_restore();
    }
}

static err_t wireguard_netif_init(struct netif *netif)
{
    if (wireguard_pending_init_data == NULL) {
        return ERR_ARG;
    }
    return wireguardif_init_with_data(netif, wireguard_pending_init_data);
}

static void wireguard_start_tcpip(void *argument)
{
    wireguard_start_request_t *request = (wireguard_start_request_t *)argument;
    LWIP_ASSERT_CORE_LOCKED();
    request->result = ERR_ARG;

    wireguard_lock();
    const bool start_permitted = wireguard_service.desired_up &&
        !wireguard_service.suspended && wireguard_service.wifi_has_ip &&
        wireguard_service.policy == request->policy;
    wireguard_unlock();
    if (!start_permitted) {
        request->result = ERR_ABRT;
        return;
    }

    if (wireguard_service.runtime_active) {
        request->result = ERR_ALREADY;
        return;
    }

    struct wireguardif_init_data init_data = {
        .private_key = request->profile.private_key,
        .listen_port = request->profile.listen_port,
        .bind_netif = request->wifi_netif,
        .initiator_only = true,
    };
    ip4_addr_t address = {.addr = request->profile.address};
    ip4_addr_t netmask = {.addr = wireguard_prefix_mask(request->profile.address_prefix)};
    ip4_addr_t gateway = {0};

    memset(&wireguard_service.wg_netif, 0, sizeof(wireguard_service.wg_netif));
    wireguard_pending_init_data = &init_data;
    if (netif_add(&wireguard_service.wg_netif,
                  &address,
                  &netmask,
                  &gateway,
                  NULL,
                  wireguard_netif_init,
                  ip_input) == NULL) {
        wireguard_pending_init_data = NULL;
        SOLAR_OS_LOGE(TAG, "raw interface initialization failed");
        request->result = ERR_IF;
        return;
    }
    wireguard_pending_init_data = NULL;
    wireguard_service.wg_netif.mtu = request->profile.mtu;
    netif_set_up(&wireguard_service.wg_netif);

    struct wireguardif_peer peer;
    wireguardif_peer_init(&peer);
    wireguard_service.endpoint_ip = request->endpoint_ip;
    peer.public_key = request->profile.public_key;
    peer.preshared_key = request->has_preshared_key ? request->preshared_key : NULL;
    ip_addr_set_ip4_u32(&peer.allowed_ip, request->profile.routes[0].network);
    ip_addr_set_ip4_u32(&peer.allowed_mask,
                        wireguard_prefix_mask(request->profile.routes[0].prefix));
    ip_addr_set_ip4_u32(&peer.endpoint_ip, request->endpoint_ip.addr);
    peer.endport_port = request->profile.endpoint_port;
    peer.keep_alive = request->profile.keepalive_seconds;

    u8_t peer_index = WIREGUARDIF_INVALID_INDEX;
    err_t error = wireguardif_add_peer(&wireguard_service.wg_netif, &peer, &peer_index);
    if (error != ERR_OK) {
        SOLAR_OS_LOGE(TAG, "peer initialization failed: lwIP %d", (int)error);
    }
    for (size_t i = 1U; error == ERR_OK && i < request->profile.route_count; i++) {
        ip_addr_t route;
        ip_addr_t mask;
        ip_addr_set_ip4_u32(&route, request->profile.routes[i].network);
        ip_addr_set_ip4_u32(&mask, wireguard_prefix_mask(request->profile.routes[i].prefix));
        error = wireguardif_add_allowed_ip(&wireguard_service.wg_netif,
                                           peer_index,
                                           &route,
                                           &mask);
        if (error != ERR_OK) {
            SOLAR_OS_LOGE(TAG,
                          "allowed route %u initialization failed: lwIP %d",
                          (unsigned)i,
                          (int)error);
        }
    }
    if (error == ERR_OK) {
        error = wireguardif_connect(&wireguard_service.wg_netif, peer_index);
        if (error != ERR_OK) {
            SOLAR_OS_LOGE(TAG, "peer activation failed: lwIP %d", (int)error);
        }
    }
    if (error != ERR_OK) {
        (void)wireguardif_shutdown(&wireguard_service.wg_netif);
        netif_remove(&wireguard_service.wg_netif);
        memset(&wireguard_service.wg_netif, 0, sizeof(wireguard_service.wg_netif));
        request->result = error;
        return;
    }

    wireguard_service.wifi_netif = request->wifi_netif;
    wireguard_service.endpoint_ip = request->endpoint_ip;
    wireguard_service.peer_index = peer_index;
    wireguard_service.runtime_active = true;
    wireguard_service.peer_up = false;
    wireguard_service.policy = request->policy;
    wireguard_routes_configure(&request->profile);
    wireguard_apply_link_policy(false);
    request->result = ERR_OK;
}

static esp_err_t wireguard_start_runtime(void)
{
    wireguard_profile_store_t profile;
    wireguard_lock();
    if (wireguard_service.active_profile == NULL) {
        wireguard_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    profile = *wireguard_service.active_profile;
    wireguard_service.state = SOLAR_OS_WIREGUARD_STATE_RESOLVING;
    const solar_os_wireguard_policy_t policy = wireguard_service.policy;
    ip4_addr_t cached_endpoint = wireguard_service.endpoint_ip;
    wireguard_unlock();

    esp_err_t error = ESP_OK;
    ip4_addr_t endpoint_ip = cached_endpoint;
    if (endpoint_ip.addr == 0U) {
        error = wireguard_resolve_endpoint(profile.endpoint, &endpoint_ip);
    }
    esp_netif_t *wifi = solar_os_wifi_get_sta_netif();
    struct netif *wifi_netif = wifi != NULL ? esp_netif_get_netif_impl(wifi) : NULL;
    if (error != ESP_OK || wifi_netif == NULL) {
        crypto_zero(&profile, sizeof(profile));
        return error != ESP_OK ? error : ESP_ERR_INVALID_STATE;
    }

    wireguard_start_request_t *request = calloc(1U, sizeof(*request));
    if (request == NULL) {
        crypto_zero(&profile, sizeof(profile));
        return ESP_ERR_NO_MEM;
    }
    request->wifi_netif = wifi_netif;
    request->endpoint_ip = endpoint_ip;
    request->profile = profile;
    request->policy = policy;
    if (profile.preshared_key[0] != '\0') {
        size_t psk_len = sizeof(request->preshared_key);
        request->has_preshared_key =
            wireguard_base64_decode(profile.preshared_key,
                                    request->preshared_key,
                                    &psk_len) &&
            psk_len == sizeof(request->preshared_key);
        if (!request->has_preshared_key) {
            crypto_zero(request, sizeof(*request));
            free(request);
            crypto_zero(&profile, sizeof(profile));
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    const err_t callback_error = tcpip_callback_wait(wireguard_start_tcpip,
                                                     request);
    const err_t start_error = request->result;
    crypto_zero(request, sizeof(*request));
    free(request);
    crypto_zero(&profile, sizeof(profile));
    if (callback_error != ERR_OK || start_error != ERR_OK) {
        SOLAR_OS_LOGE(TAG,
                      "runtime start failed: callback=%d runtime=%d",
                      (int)callback_error,
                      (int)start_error);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void wireguard_poll_peer(void *argument)
{
    bool *peer_up = (bool *)argument;
    LWIP_ASSERT_CORE_LOCKED();
    *peer_up = wireguard_service.runtime_active &&
        wireguardif_peer_is_up(&wireguard_service.wg_netif,
                               wireguard_service.peer_index,
                               NULL,
                               NULL) == ERR_OK;
    wireguard_apply_link_policy(*peer_up);
}

static void wireguard_worker(void *argument)
{
    (void)argument;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500U));

        wireguard_lock();
        const bool stop_requested = wireguard_service.worker_stop_requested;
        const bool should_run = wireguard_service.desired_up &&
            !wireguard_service.suspended && wireguard_service.wifi_has_ip;
        const bool runtime_active = wireguard_service.runtime_active;
        const uint32_t now_ms = (uint32_t)(solar_os_time_uptime_ms() & UINT32_MAX);
        const bool retry_due = wireguard_service.last_retry_ms == 0U ||
            (now_ms - wireguard_service.last_retry_ms) >= WIREGUARD_RETRY_MS;
        wireguard_unlock();
        if (stop_requested) {
            break;
        }

        if (should_run && !runtime_active && retry_due) {
            wireguard_lock();
            wireguard_service.last_retry_ms = now_ms;
            wireguard_unlock();
            const esp_err_t error = wireguard_start_runtime();
            wireguard_lock();
            const bool still_requested = !wireguard_service.worker_stop_requested &&
                wireguard_service.desired_up && !wireguard_service.suspended;
            if (still_requested) {
                wireguard_service.last_error = error;
                wireguard_service.state = error == ESP_OK ?
                    SOLAR_OS_WIREGUARD_STATE_CONNECTING :
                    SOLAR_OS_WIREGUARD_STATE_ERROR;
            }
            wireguard_unlock();
        }

        wireguard_lock();
        const bool stop_before_poll = wireguard_service.worker_stop_requested;
        const bool poll = !stop_before_poll && wireguard_service.runtime_active;
        wireguard_unlock();
        if (stop_before_poll) {
            break;
        }
        if (poll) {
            bool peer_up = false;
            if (tcpip_callback_wait(wireguard_poll_peer, &peer_up) == ERR_OK) {
                wireguard_lock();
                if (!wireguard_service.worker_stop_requested) {
                    wireguard_service.peer_up = peer_up;
                    wireguard_service.state = peer_up ?
                        SOLAR_OS_WIREGUARD_STATE_UP :
                        SOLAR_OS_WIREGUARD_STATE_CONNECTING;
                }
                wireguard_unlock();
            }
        }
    }

    wireguard_lock();
    wireguard_service.worker_done = true;
    wireguard_unlock();
    solar_os_task_delete_external(NULL);
}

static esp_err_t wireguard_worker_start(void)
{
    wireguard_lock();
    if (wireguard_service.worker != NULL) {
        const esp_err_t error = wireguard_service.worker_stop_requested ?
            ESP_ERR_INVALID_STATE : ESP_OK;
        wireguard_unlock();
        return error;
    }

    wireguard_service.worker_stop_requested = false;
    wireguard_service.worker_done = false;
    /* This worker owns no flash operations. The NVS profile is loaded by the
     * caller before launch, so its stack can use PSRAM safely. */
    const BaseType_t created = solar_os_task_create_pinned_external(
        wireguard_worker,
        "wireguard",
        WIREGUARD_WORKER_STACK,
        NULL,
        WIREGUARD_WORKER_PRIORITY,
        &wireguard_service.worker,
        tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_SYSTEM);
    if (created != pdPASS) {
        wireguard_service.worker = NULL;
        wireguard_service.worker_done = true;
    }
    wireguard_unlock();
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t wireguard_worker_stop(void)
{
    wireguard_lock();
    TaskHandle_t worker = wireguard_service.worker;
    if (worker != NULL && !wireguard_service.worker_stop_requested) {
        wireguard_service.worker_stop_requested = true;
        /* Keep the mutex until the notification is sent. The worker cannot
         * observe the stop request and self-delete while its handle is used. */
        xTaskNotifyGive(worker);
    }
    wireguard_unlock();

    if (worker == NULL) {
        return ESP_OK;
    }
    if (!solar_os_task_wait_done(worker,
                                 &wireguard_service.worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        SOLAR_OS_LOGE(TAG,
                      "worker did not stop within %u ms",
                      (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
        return ESP_ERR_TIMEOUT;
    }

    wireguard_lock();
    if (wireguard_service.worker == worker) {
        wireguard_service.worker = NULL;
    }
    wireguard_service.worker_stop_requested = false;
    wireguard_service.worker_done = false;
    wireguard_unlock();
    return ESP_OK;
}

static void wireguard_handle_wifi_lost(void)
{
    wireguard_lock();
    wireguard_service.wifi_has_ip = false;
    const bool desired = wireguard_service.desired_up;
    const bool preserve = desired &&
        wireguard_service.policy == SOLAR_OS_WIREGUARD_POLICY_FAIL_CLOSED;
    wireguard_service.state = desired ?
        SOLAR_OS_WIREGUARD_STATE_WAIT_WIFI : SOLAR_OS_WIREGUARD_STATE_OFF;
    wireguard_unlock();
    (void)tcpip_callback_wait(wireguard_stop_tcpip,
                              preserve ? &wireguard_service : NULL);
}

static void wireguard_network_event(void *argument,
                                    esp_event_base_t event_base,
                                    int32_t event_id,
                                    void *event_data)
{
    (void)argument;
    (void)event_data;
    if (event_base == WIFI_EVENT &&
        (event_id == WIFI_EVENT_STA_DISCONNECTED || event_id == WIFI_EVENT_STA_STOP)) {
        wireguard_handle_wifi_lost();
        return;
    }
    if (event_base != IP_EVENT) {
        return;
    }

    if (event_id == IP_EVENT_STA_GOT_IP) {
        wireguard_lock();
        wireguard_service.wifi_has_ip = true;
        const bool wake = wireguard_service.desired_up && !wireguard_service.suspended;
        const bool fail_closed = wake &&
            wireguard_service.policy == SOLAR_OS_WIREGUARD_POLICY_FAIL_CLOSED;
        if (wake) {
            wireguard_service.state = SOLAR_OS_WIREGUARD_STATE_RESOLVING;
            wireguard_service.last_retry_ms = 0U;
        }
        wireguard_unlock();
        if (fail_closed) {
            const esp_err_t filter_error = wireguard_enforce_fail_closed();
            if (filter_error != ESP_OK) {
                wireguard_lock();
                wireguard_service.desired_up = false;
                wireguard_service.state = SOLAR_OS_WIREGUARD_STATE_ERROR;
                wireguard_service.last_error = filter_error;
                wireguard_unlock();
                return;
            }
        }
        wireguard_lock();
        if (wireguard_service.desired_up && !wireguard_service.suspended &&
            wireguard_service.wifi_has_ip && wireguard_service.worker != NULL &&
            !wireguard_service.worker_stop_requested) {
            xTaskNotifyGive(wireguard_service.worker);
        }
        wireguard_unlock();
    } else if (event_id == IP_EVENT_STA_LOST_IP) {
        wireguard_handle_wifi_lost();
    }
}

esp_err_t solar_os_wireguard_init(void)
{
    if (wireguard_service.initialized) {
        return ESP_OK;
    }
    memset(&wireguard_service, 0, sizeof(wireguard_service));
    wireguard_service.peer_index = WIREGUARDIF_INVALID_INDEX;
    wireguard_service.state = SOLAR_OS_WIREGUARD_STATE_OFF;
    wireguard_service.policy = SOLAR_OS_WIREGUARD_POLICY_AUTO;
    wireguard_service.mutex = xSemaphoreCreateMutex();
    if (wireguard_service.mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t event_loop_error = esp_event_loop_create_default();
    if (event_loop_error != ESP_OK && event_loop_error != ESP_ERR_INVALID_STATE) {
        return event_loop_error;
    }
    esp_err_t error = esp_event_handler_instance_register(IP_EVENT,
                                                          IP_EVENT_STA_GOT_IP,
                                                          wireguard_network_event,
                                                          NULL,
                                                          NULL);
    if (error == ESP_OK) {
        error = esp_event_handler_instance_register(IP_EVENT,
                                                    IP_EVENT_STA_LOST_IP,
                                                    wireguard_network_event,
                                                    NULL,
                                                    NULL);
    }
    if (error == ESP_OK) {
        error = esp_event_handler_instance_register(WIFI_EVENT,
                                                    WIFI_EVENT_STA_DISCONNECTED,
                                                    wireguard_network_event,
                                                    NULL,
                                                    NULL);
    }
    if (error == ESP_OK) {
        error = esp_event_handler_instance_register(WIFI_EVENT,
                                                    WIFI_EVENT_STA_STOP,
                                                    wireguard_network_event,
                                                    NULL,
                                                    NULL);
    }
    if (error != ESP_OK) {
        return error;
    }

    error = wireguard_time_reserve();
    wireguard_service.time_ready = error == ESP_OK;
    if (error != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "timestamp reservation failed: %s", esp_err_to_name(error));
    }

    wireguard_profile_store_t profile;
    if (wireguard_profile_load(&profile) == ESP_OK) {
        wireguard_summary_set(&profile);
        crypto_zero(&profile, sizeof(profile));
    }

    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    wireguard_service.wifi_has_ip = wifi.has_ip;
    wireguard_service.initialized = true;
    return ESP_OK;
}

esp_err_t solar_os_wireguard_import(const char *path, char *detail, size_t detail_len)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wireguard_service.initialized) {
        const esp_err_t init_error = solar_os_wireguard_init();
        if (init_error != ESP_OK) {
            return init_error;
        }
    }
    wireguard_lock();
    const bool busy = wireguard_service.desired_up ||
        wireguard_service.runtime_active || wireguard_service.worker != NULL;
    wireguard_unlock();
    if (busy) {
        wireguard_set_detail(detail, detail_len, 0U, "bring the tunnel down before import");
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        wireguard_set_detail(detail, detail_len, 0U, "cannot open configuration file");
        return ESP_ERR_NOT_FOUND;
    }
    wireguard_profile_store_t *profile = calloc(1U, sizeof(*profile));
    if (profile == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = wireguard_parse_config(file, profile, detail, detail_len);
    fclose(file);
    if (error == ESP_OK) {
        error = wireguard_profile_save(profile);
        if (error == ESP_OK) {
            wireguard_summary_set(profile);
            wireguard_set_detail(detail, detail_len, 0U,
                                 "profile imported; source file was not removed");
        }
    }
    crypto_zero(profile, sizeof(*profile));
    free(profile);
    return error;
}

esp_err_t solar_os_wireguard_forget(void)
{
    if (!wireguard_service.initialized) {
        const esp_err_t init_error = solar_os_wireguard_init();
        if (init_error != ESP_OK) {
            return init_error;
        }
    }
    wireguard_lock();
    const bool busy = wireguard_service.desired_up ||
        wireguard_service.runtime_active || wireguard_service.worker != NULL;
    wireguard_unlock();
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t error = wireguard_profile_erase();
    if (error != ESP_OK) {
        return error;
    }
    wireguard_summary_set(NULL);
    wireguard_lock();
    wireguard_service.policy = SOLAR_OS_WIREGUARD_POLICY_AUTO;
    wireguard_service.state = SOLAR_OS_WIREGUARD_STATE_OFF;
    wireguard_service.last_error = ESP_OK;
    wireguard_unlock();
    return ESP_OK;
}

esp_err_t solar_os_wireguard_up(solar_os_wireguard_policy_t policy)
{
    if (!wireguard_service.initialized) {
        const esp_err_t init_error = solar_os_wireguard_init();
        if (init_error != ESP_OK) {
            return init_error;
        }
    }
    wireguard_lock();
    if (!wireguard_service.configured || !wireguard_service.time_ready) {
        wireguard_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (policy == SOLAR_OS_WIREGUARD_POLICY_AUTO) {
        policy = wireguard_service.full_tunnel ?
            SOLAR_OS_WIREGUARD_POLICY_FAIL_CLOSED :
            SOLAR_OS_WIREGUARD_POLICY_FAIL_OPEN;
    }
    if (policy != SOLAR_OS_WIREGUARD_POLICY_FAIL_OPEN &&
        policy != SOLAR_OS_WIREGUARD_POLICY_FAIL_CLOSED) {
        wireguard_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    const bool had_active_profile = wireguard_service.active_profile != NULL;
    wireguard_unlock();

    esp_err_t error = wireguard_active_profile_prepare();
    if (error != ESP_OK) {
        return error;
    }
    error = wireguard_worker_start();
    if (error != ESP_OK) {
        if (!had_active_profile) {
            wireguard_active_profile_clear();
        }
        return error;
    }

    wireguard_lock();
    wireguard_service.policy = policy;
    wireguard_service.desired_up = true;
    wireguard_service.suspended = false;
    wireguard_service.last_error = ESP_OK;
    wireguard_service.last_retry_ms = 0U;
    wireguard_service.state = wireguard_service.wifi_has_ip ?
        SOLAR_OS_WIREGUARD_STATE_RESOLVING :
        SOLAR_OS_WIREGUARD_STATE_WAIT_WIFI;
    const bool notify = wireguard_service.wifi_has_ip;
    const bool fail_closed = policy == SOLAR_OS_WIREGUARD_POLICY_FAIL_CLOSED;
    wireguard_unlock();
    if (fail_closed) {
        const esp_err_t filter_error = wireguard_enforce_fail_closed();
        if (filter_error != ESP_OK && notify) {
            wireguard_lock();
            wireguard_service.desired_up = false;
            wireguard_service.state = SOLAR_OS_WIREGUARD_STATE_ERROR;
            wireguard_service.last_error = filter_error;
            wireguard_unlock();
            if (wireguard_worker_stop() == ESP_OK) {
                wireguard_active_profile_clear();
            }
            return filter_error;
        }
    }
    wireguard_lock();
    if (wireguard_service.desired_up && wireguard_service.wifi_has_ip &&
        wireguard_service.worker != NULL &&
        !wireguard_service.worker_stop_requested) {
        xTaskNotifyGive(wireguard_service.worker);
    }
    wireguard_unlock();
    return ESP_OK;
}

esp_err_t solar_os_wireguard_down(void)
{
    if (!wireguard_service.initialized) {
        return ESP_OK;
    }
    wireguard_lock();
    wireguard_service.desired_up = false;
    wireguard_service.suspended = false;
    wireguard_unlock();
    const esp_err_t worker_error = wireguard_worker_stop();
    const err_t error = tcpip_callback_wait(wireguard_stop_tcpip, NULL);
    if (worker_error == ESP_OK) {
        wireguard_active_profile_clear();
    }
    wireguard_lock();
    const esp_err_t result = worker_error != ESP_OK ? worker_error :
        (error == ERR_OK ? ESP_OK : ESP_FAIL);
    wireguard_service.state = result == ESP_OK ?
        SOLAR_OS_WIREGUARD_STATE_OFF : SOLAR_OS_WIREGUARD_STATE_ERROR;
    wireguard_service.last_error = result;
    wireguard_unlock();
    return result;
}

esp_err_t solar_os_wireguard_prepare_sleep(void)
{
    if (!wireguard_service.initialized) {
        return ESP_OK;
    }
    wireguard_lock();
    const bool desired = wireguard_service.desired_up;
    const bool preserve = desired &&
        wireguard_service.policy == SOLAR_OS_WIREGUARD_POLICY_FAIL_CLOSED;
    wireguard_service.suspended = true;
    if (desired) {
        wireguard_service.state = SOLAR_OS_WIREGUARD_STATE_SUSPENDED;
    }
    wireguard_unlock();
    if (!desired) {
        return ESP_OK;
    }
    return tcpip_callback_wait(wireguard_stop_tcpip,
                               preserve ? &wireguard_service : NULL) == ERR_OK ?
        ESP_OK : ESP_FAIL;
}

esp_err_t solar_os_wireguard_resume(void)
{
    if (!wireguard_service.initialized) {
        return ESP_OK;
    }
    wireguard_lock();
    wireguard_service.suspended = false;
    const bool notify = wireguard_service.desired_up && wireguard_service.wifi_has_ip;
    if (wireguard_service.desired_up) {
        wireguard_service.state = wireguard_service.wifi_has_ip ?
            SOLAR_OS_WIREGUARD_STATE_RESOLVING :
            SOLAR_OS_WIREGUARD_STATE_WAIT_WIFI;
        wireguard_service.last_retry_ms = 0U;
    }
    if (notify && wireguard_service.worker != NULL &&
        !wireguard_service.worker_stop_requested) {
        xTaskNotifyGive(wireguard_service.worker);
    }
    wireguard_unlock();
    return ESP_OK;
}

void solar_os_wireguard_get_status(solar_os_wireguard_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));

    wireguard_lock();
    status->state = wireguard_service.state;
    status->policy = wireguard_service.policy;
    status->initialized = wireguard_service.initialized;
    status->configured = wireguard_service.configured;
    status->desired_up = wireguard_service.desired_up;
    status->peer_up = wireguard_service.peer_up;
    status->full_tunnel = wireguard_service.full_tunnel;
    status->routes_active = wireguard_service.runtime_active;
    status->default_route_active = wireguard_service.runtime_active &&
        wireguard_service.full_tunnel &&
        (wireguard_service.peer_up ||
         wireguard_service.policy == SOLAR_OS_WIREGUARD_POLICY_FAIL_CLOSED);
    status->kill_switch_active = wireguard_service.kill_switch_active;
    status->dns_configured = wireguard_service.summary.dns_valid != 0U;
    status->route_count = wireguard_service.summary.route_count;
    status->endpoint_port = wireguard_service.summary.endpoint_port;
    status->listen_port = wireguard_service.summary.listen_port;
    status->keepalive_seconds = wireguard_service.summary.keepalive_seconds;
    status->mtu = wireguard_service.summary.mtu;
    status->last_error = wireguard_service.last_error;
    strlcpy(status->endpoint,
            wireguard_service.summary.endpoint,
            sizeof(status->endpoint));
    strlcpy(status->peer_key_fingerprint,
            wireguard_service.peer_key_fingerprint,
            sizeof(status->peer_key_fingerprint));
    ip4addr_ntoa_r(&wireguard_service.endpoint_ip,
                   status->endpoint_ip,
                   sizeof(status->endpoint_ip));
    ip4_addr_t address = {.addr = wireguard_service.summary.address};
    char address_text[16];
    ip4addr_ntoa_r(&address, address_text, sizeof(address_text));
    snprintf(status->address,
             sizeof(status->address),
             "%s/%u",
             address_text,
             (unsigned)wireguard_service.summary.address_prefix);
    if (wireguard_service.summary.dns_valid != 0U) {
        ip4_addr_t dns = {.addr = wireguard_service.summary.dns};
        ip4addr_ntoa_r(&dns, status->dns, sizeof(status->dns));
    }
    wireguard_unlock();
}

const char *solar_os_wireguard_state_name(solar_os_wireguard_state_t state)
{
    switch (state) {
    case SOLAR_OS_WIREGUARD_STATE_OFF: return "off";
    case SOLAR_OS_WIREGUARD_STATE_WAIT_WIFI: return "waiting for Wi-Fi";
    case SOLAR_OS_WIREGUARD_STATE_RESOLVING: return "resolving endpoint";
    case SOLAR_OS_WIREGUARD_STATE_CONNECTING: return "connecting";
    case SOLAR_OS_WIREGUARD_STATE_UP: return "up";
    case SOLAR_OS_WIREGUARD_STATE_SUSPENDED: return "suspended";
    case SOLAR_OS_WIREGUARD_STATE_ERROR: return "error";
    default: return "unknown";
    }
}

const char *solar_os_wireguard_policy_name(solar_os_wireguard_policy_t policy)
{
    switch (policy) {
    case SOLAR_OS_WIREGUARD_POLICY_FAIL_OPEN: return "fail-open";
    case SOLAR_OS_WIREGUARD_POLICY_FAIL_CLOSED: return "fail-closed";
    case SOLAR_OS_WIREGUARD_POLICY_AUTO:
    default: return "auto";
    }
}

uint32_t wireguard_sys_now(void)
{
    return (uint32_t)(solar_os_time_uptime_ms() & UINT32_MAX);
}

void wireguard_random_bytes(void *bytes, size_t size)
{
    esp_fill_random(bytes, size);
}

void wireguard_tai64n_now(uint8_t *output)
{
    const uint64_t elapsed_ms = solar_os_time_uptime_ms() -
        wireguard_service.tai_started_ms;
    const uint64_t seconds = WIREGUARD_TAI64_EPOCH_OFFSET +
        wireguard_service.tai_base_seconds + elapsed_ms / 1000ULL;
    const uint32_t nanoseconds = (uint32_t)(elapsed_ms % 1000ULL) * 1000000UL;
    for (size_t i = 0; i < 8U; i++) {
        output[i] = (uint8_t)(seconds >> ((7U - i) * 8U));
    }
    for (size_t i = 0; i < 4U; i++) {
        output[8U + i] = (uint8_t)(nanoseconds >> ((3U - i) * 8U));
    }
}

bool wireguard_is_under_load(void)
{
    return false;
}
