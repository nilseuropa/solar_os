/* Exercise the actual Wi-Fi owner registry and policy transitions. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef int wifi_ps_type_t;
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM, ESP_FAIL };
enum { WIFI_PS_NONE, WIFI_PS_MAX_MODEM };
static bool wifi_initialized = true, wifi_started = true, wifi_connectionless_active;
static bool repeater, locked, fail_next;
static unsigned calls;
static wifi_ps_type_t current_mode = WIFI_PS_MAX_MODEM;
#define SOLAR_OS_LOGI(...) ((void)0)
static void wifi_lock(void) { assert(!locked); locked = true; }
static void wifi_unlock(void) { assert(locked); locked = false; }
static bool solar_os_wifi_repeater_is_enabled(void) { return repeater; }
static esp_err_t esp_wifi_set_ps(wifi_ps_type_t mode)
{
    assert(locked); calls++;
    if (fail_next) { fail_next = false; return ESP_FAIL; }
    current_mode = mode; return ESP_OK;
}
static size_t strlcpy(char *dst, const char *src, size_t size)
{ size_t n = strlen(src); if (size) snprintf(dst, size, "%s", src); return n; }
#include "wifi_latency_production.h"
int main(void)
{
    assert(solar_os_wifi_latency_acquire(NULL) == ESP_ERR_INVALID_ARG);
    assert(solar_os_wifi_latency_acquire("") == ESP_ERR_INVALID_ARG);
    char long_owner[SOLAR_OS_WIFI_LATENCY_OWNER_MAX + 1];
    memset(long_owner, 'x', sizeof(long_owner)); long_owner[sizeof(long_owner) - 1] = 0;
    assert(solar_os_wifi_latency_acquire(long_owner) == ESP_ERR_INVALID_ARG);
    wifi_started = false;
    assert(solar_os_wifi_latency_acquire("rtsp") == ESP_ERR_INVALID_STATE);
    wifi_started = true;
    assert(solar_os_wifi_latency_acquire("telnet") == ESP_OK);
    assert(solar_os_wifi_latency_acquire("rtsp") == ESP_OK);
    unsigned before = calls;
    assert(solar_os_wifi_latency_acquire("rtsp") == ESP_OK && calls == before);
    assert(wifi_latency_count == 2 && current_mode == WIFI_PS_NONE);
    assert(solar_os_wifi_latency_release("unknown") == ESP_ERR_INVALID_STATE);
    assert(solar_os_wifi_latency_release("telnet") == ESP_OK);
    assert(wifi_latency_count == 1 && current_mode == WIFI_PS_NONE);
    fail_next = true;
    assert(solar_os_wifi_latency_acquire("failed") == ESP_FAIL);
    assert(wifi_latency_count == 1 && current_mode == WIFI_PS_NONE);
    assert(solar_os_wifi_latency_release("rtsp") == ESP_OK);
    assert(!wifi_latency_count && current_mode == WIFI_PS_MAX_MODEM);
    assert(solar_os_wifi_latency_release("rtsp") == ESP_OK);
    fail_next = true;
    assert(solar_os_wifi_latency_acquire("failed") == ESP_FAIL);
    assert(!wifi_latency_count && current_mode == WIFI_PS_MAX_MODEM);
    for (unsigned i = 0; i < SOLAR_OS_WIFI_LATENCY_OWNERS_MAX; i++) {
        char owner[16]; snprintf(owner, sizeof(owner), "owner%u", i);
        assert(solar_os_wifi_latency_acquire(owner) == ESP_OK);
    }
    assert(solar_os_wifi_latency_acquire("overflow") == ESP_ERR_NO_MEM);
    for (unsigned i = 0; i < SOLAR_OS_WIFI_LATENCY_OWNERS_MAX; i++) {
        char owner[16]; snprintf(owner, sizeof(owner), "owner%u", i);
        assert(solar_os_wifi_latency_release(owner) == ESP_OK);
        assert(current_mode == (wifi_latency_count ? WIFI_PS_NONE : WIFI_PS_MAX_MODEM));
    }
    repeater = true;
    assert(solar_os_wifi_latency_acquire("rtsp") == ESP_OK);
    assert(solar_os_wifi_latency_release("rtsp") == ESP_OK && current_mode == WIFI_PS_NONE);
    repeater = false; wifi_connectionless_active = true;
    assert(solar_os_wifi_latency_acquire("rtsp") == ESP_OK);
    assert(solar_os_wifi_latency_release("rtsp") == ESP_OK && current_mode == WIFI_PS_NONE);
    wifi_connectionless_active = false;
    assert(solar_os_wifi_latency_acquire("rtsp") == ESP_OK);
    fail_next = true;
    assert(solar_os_wifi_latency_release("rtsp") == ESP_FAIL);
    assert(!wifi_latency_count);
    assert(solar_os_wifi_latency_acquire("rtsp") == ESP_OK);
    assert(solar_os_wifi_latency_release("rtsp") == ESP_OK && current_mode == WIFI_PS_MAX_MODEM);
    assert(solar_os_wifi_latency_acquire("rtsp") == ESP_OK);
    wifi_started = false; before = calls;
    assert(solar_os_wifi_latency_release("rtsp") == ESP_OK && calls == before);
    assert(!wifi_latency_count && !locked);
    puts("Wi-Fi latency ownership tests: OK");
}
