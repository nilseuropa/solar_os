#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"

#define BLE_KEYBOARD_DISCOVERY_PAUSE_TIMEOUT_MS 1500U
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
enum { BLE_KEYBOARD_IDLE, BLE_KEYBOARD_SCANNING, BLE_KEYBOARD_CONNECTING,
       BLE_KEYBOARD_CONNECTED, BLE_KEYBOARD_PASSKEY };
enum { BLE_KEYBOARD_SCAN_DISCOVERY };
typedef struct { unsigned device; } solar_os_ble_keyboard_scan_result_t;
static bool initialized, connected, reconnect_suppressed_for_client;
static uint32_t client_leases;
static int client_lease_lock, state, active_scan_mode;
static void *scan_task_handle, *reconnect_task_handle;
static solar_os_ble_keyboard_scan_result_t *active_scan_results;
static size_t active_scan_max_results, active_scan_result_count;
static bool stop_succeeds;
static esp_err_t scan_result;
static size_t advertised;
static unsigned stops, scans, restarts, restores, statuses;
static int worker;

static bool stop_reconnect_task(const char *reason, uint32_t timeout)
{
    assert(!strcmp(reason, "GATT client"));
    assert(timeout == BLE_KEYBOARD_DISCOVERY_PAUSE_TIMEOUT_MS);
    assert(client_leases && reconnect_suppressed_for_client);
    stops++;
    if (!stop_succeeds) return false;
    if (reconnect_task_handle) {
        reconnect_task_handle = NULL;
        if (state == BLE_KEYBOARD_SCANNING) state = BLE_KEYBOARD_IDLE;
    }
    return true;
}

static void schedule_reconnect(uint32_t delay)
{
    assert(!delay);
    if (connected || scan_task_handle || reconnect_suppressed_for_client) return;
    /* A restarted worker must never retain the discovery caller's buffer. */
    assert(!active_scan_results && !active_scan_max_results && !active_scan_result_count);
    reconnect_task_handle = &worker;
    restarts++;
}

static void collect_connected_scan_result(void)
{
    if (connected) active_scan_results[active_scan_result_count++].device = 99;
}

static esp_err_t run_keyboard_scan(int mode, void *candidate)
{
    assert(mode == BLE_KEYBOARD_SCAN_DISCOVERY && !candidate);
    assert(client_leases && reconnect_suppressed_for_client && !reconnect_task_handle);
    assert(active_scan_results && active_scan_max_results == 4);
    scans++;
    /* A reconnect request during discovery must stay suppressed. */
    schedule_reconnect(0);
    for (size_t i = 0; i < advertised; i++)
        active_scan_results[active_scan_result_count++].device = 73 + i;
    return scan_result;
}

static void set_status(int next, const char *text)
{
    assert(next == BLE_KEYBOARD_IDLE && !strcmp(text, "scan done"));
    assert(client_leases && reconnect_suppressed_for_client);
    state = next;
    statuses++;
}

static void restore_status_after_scan(int mode)
{
    assert(mode == BLE_KEYBOARD_SCAN_DISCOVERY && connected);
    assert(client_leases && reconnect_suppressed_for_client);
    state = BLE_KEYBOARD_CONNECTED;
    restores++;
}

#include "ble_discovery_functions.inc"

static void reset(void)
{
    assert(!client_leases && !active_scan_results);
    initialized = true; connected = false; reconnect_suppressed_for_client = false;
    state = BLE_KEYBOARD_IDLE;
    scan_task_handle = reconnect_task_handle = NULL;
    active_scan_max_results = active_scan_result_count = 0;
    stop_succeeds = true; scan_result = ESP_OK; advertised = 1;
    stops = scans = restarts = restores = statuses = 0;
}

int main(void)
{
    solar_os_ble_keyboard_scan_result_t results[4];
    size_t found;
    /* A remembered, offline keyboard has a persistent worker while scanning
     * and during backoff. Explicit discovery must work in both states. */
    for (unsigned scanning = 0; scanning < 2; scanning++) {
        reset(); reconnect_task_handle = &worker;
        state = scanning ? BLE_KEYBOARD_SCANNING : BLE_KEYBOARD_IDLE;
        assert(solar_os_ble_backend_scan(results, 4, &found) == ESP_OK);
        assert(found == 1 && results[0].device == 73);
        assert(stops == 1 && scans == 1 && restarts == 1 && statuses == 1);
        assert(!client_leases && !reconnect_suppressed_for_client && reconnect_task_handle);
    }
    /* A connected keyboard is retained and included in discovery results. */
    reset(); connected = true; state = BLE_KEYBOARD_CONNECTED;
    assert(solar_os_ble_backend_scan(results, 4, &found) == ESP_OK);
    assert(found == 2 && results[0].device == 99 && results[1].device == 73);
    assert(restores == 1 && !restarts && connected && state == BLE_KEYBOARD_CONNECTED);

    const esp_err_t errors[] = {ESP_ERR_NOT_FOUND, ESP_ERR_TIMEOUT, ESP_FAIL};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        reset(); reconnect_task_handle = &worker; advertised = 0; scan_result = errors[i];
        assert(solar_os_ble_backend_scan(results, 4, &found) == errors[i]);
        assert(!found && !active_scan_results && !client_leases);
        assert(scans == 1 && restarts == 1 && !reconnect_suppressed_for_client);
    }
    /* Discovery includes non-keyboard advertisements even if no keyboard
     * candidate was selected by the shared scanner. */
    reset(); advertised = 2; scan_result = ESP_ERR_NOT_FOUND;
    assert(solar_os_ble_backend_scan(results, 4, &found) == ESP_OK);
    assert(found == 2 && statuses == 1 && restarts == 1 && !client_leases);

    /* Explicit pairing and an uncancelled HID open must still block safely. */
    reset(); scan_task_handle = &worker; state = BLE_KEYBOARD_SCANNING;
    assert(solar_os_ble_backend_scan(results, 4, &found) == ESP_ERR_INVALID_STATE);
    assert(!found && !scans && !client_leases && !reconnect_suppressed_for_client);
    reset(); reconnect_task_handle = &worker; stop_succeeds = false;
    assert(solar_os_ble_backend_scan(results, 4, &found) == ESP_ERR_TIMEOUT);
    assert(!found && !scans && !client_leases && !reconnect_suppressed_for_client);

    reset(); initialized = false;
    assert(solar_os_ble_backend_scan(results, 4, &found) == ESP_ERR_INVALID_STATE);
    assert(!found && !stops);
    reset();
    assert(solar_os_ble_backend_scan(NULL, 4, &found) == ESP_ERR_INVALID_ARG);
    assert(solar_os_ble_backend_scan(results, 0, &found) == ESP_ERR_INVALID_ARG);
    assert(solar_os_ble_backend_scan(results, 4, NULL) == ESP_ERR_INVALID_ARG);
    assert(!stops && !scans);
    puts("BLE discovery: offline reconnect preemption, result lifetime, errors, connected keyboard and pairing PASS");
}
