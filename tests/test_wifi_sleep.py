from pathlib import Path
import unittest

from tests.test_foreground_presenter import production_functions
from tests.test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class WifiSleepTest(unittest.TestCase):
    def test_post_wake_association_failures_retry_and_explicit_disconnect_cancels(self):
        source = (ROOT / "src/services/solar_os_wifi.c").read_text()
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#define ESP_ERR_WIFI_NOT_STARTED 1
#define ESP_ERR_WIFI_NOT_CONNECT 2
#define WIFI_REPEATER_RECONNECT_INITIAL_MS 1000U
#define WIFI_REPEATER_RECONNECT_MAX_MS 30000U
enum { SOLAR_OS_WIFI_STATE_DISCONNECTED, SOLAR_OS_WIFI_STATE_CONNECTING,
       SOLAR_OS_WIFI_STATE_IDLE };
static bool wifi_initialized = true, wifi_started = true, wifi_sta_enabled = true;
static bool wifi_suspended, wifi_connected, repeater, timer_active;
static uint8_t wifi_resume_reconnect_attempts = 3;
static uint8_t wifi_disconnect_reason;
static uint32_t wifi_repeater_reconnect_delay_ms = 1000;
static void *wifi_repeater_reconnect_timer = (void *)1;
static int wifi_state;
static unsigned schedules, connects;
static bool connect_fails;
static bool solar_os_wifi_repeater_is_enabled(void) { return repeater; }
static void wifi_lock(void) {}
static void wifi_unlock(void) {}
static void wifi_clear_link_state(void) { wifi_connected = false; }
static esp_err_t esp_wifi_disconnect(void) { return ESP_OK; }
static esp_err_t esp_wifi_connect(void) { connects++; return connect_fails ? ESP_FAIL : ESP_OK; }
static bool esp_timer_is_active(void *timer) { assert(timer); return timer_active; }
static esp_err_t esp_timer_stop(void *timer) { assert(timer); timer_active = false; return ESP_OK; }
static esp_err_t esp_timer_start_once(void *timer, uint64_t delay) {
    assert(timer && delay >= 1000000 && delay <= 30000000);
    timer_active = true; schedules++; return ESP_OK;
}
static void wifi_repeater_schedule_reconnect(void);
'''
        harness += production_functions(source, (
            "wifi_repeater_cancel_reconnect", "wifi_repeater_reconnect_callback",
            "wifi_repeater_schedule_reconnect", "solar_os_wifi_disconnect"))
        harness += r'''
static void disconnected(void) {
    wifi_state = SOLAR_OS_WIFI_STATE_DISCONNECTED;
    wifi_repeater_schedule_reconnect();
}
static void fire_timer(void) {
    assert(timer_active); timer_active = false;
    wifi_repeater_reconnect_callback(NULL);
}
int main(void) {
    /* Driver disconnects after connect() returned success must still retry. */
    for (unsigned i = 0; i < 3; i++) {
        disconnected(); assert(timer_active);
        fire_timer(); assert(connects == i + 1);
    }
    disconnected(); assert(!timer_active && schedules == 3);
    /* Immediate failures in a timer callback also have a bounded retry budget. */
    wifi_resume_reconnect_attempts = 3; connect_fails = true;
    disconnected();
    for (unsigned i = 0; i < 3; i++) fire_timer();
    assert(!timer_active && connects == 6);
    /* The existing success/stop/sleep cancellation resets both delay and budget. */
    wifi_resume_reconnect_attempts = 3;
    disconnected();
    wifi_repeater_cancel_reconnect();
    assert(!timer_active && !wifi_resume_reconnect_attempts);
    assert(wifi_repeater_reconnect_delay_ms == 1000);
    /* A user disconnect must not be undone by a pending wake retry. */
    wifi_resume_reconnect_attempts = 3;
    disconnected();
    assert(solar_os_wifi_disconnect() == ESP_OK);
    disconnected();
    assert(!timer_active && !wifi_resume_reconnect_attempts);
    /* Repeaters keep their pre-existing ongoing reconnect policy. */
    repeater = true;
    disconnected(); assert(timer_active);
    fire_timer(); assert(timer_active && connects == 7);
}
'''
        run_c(harness)

    def test_delayed_stop_events_and_failed_resume_preserve_requested_mode(self):
        source = (ROOT / "src/services/solar_os_wifi.c").read_text()
        stop_event = source.split("case WIFI_EVENT_STA_STOP:", 1)[1].split(
            "case WIFI_EVENT_AP_START:", 1)[0].rsplit("break;", 1)[0]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#define ESP_ERR_WIFI_NOT_STARTED 0x3002
#define SOLAR_OS_LOGI(...) ((void)0)
#define SOLAR_OS_LOGW(...) ((void)0)
#define WIFI_SECOND_CHAN_NONE 0
#define WIFI_RESUME_RECONNECT_ATTEMPTS 3U
typedef enum { WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA } wifi_mode_t;
enum { SOLAR_OS_WIFI_STATE_OFF, SOLAR_OS_WIFI_STATE_IDLE,
       SOLAR_OS_WIFI_STATE_CONNECTING, SOLAR_OS_WIFI_STATE_DISCONNECTED,
       SOLAR_OS_WIFI_STATE_FAILED };
static bool wifi_initialized = true, wifi_suspended, wifi_started;
static bool wifi_sta_enabled, wifi_ap_enabled, wifi_connected, wifi_has_ip;
static bool wifi_sleep_was_started, wifi_sleep_sta_enabled;
static bool wifi_sleep_ap_enabled, wifi_sleep_reconnect_sta;
static bool wifi_ap_running, wifi_nat_active, wifi_connectionless_active;
static unsigned wifi_ap_station_count;
static uint8_t wifi_connectionless_channel;
static uint8_t wifi_resume_reconnect_attempts;
static int wifi_state;
static void *wifi_sta_netif;
static unsigned starts, stops, connects, channels;
static unsigned start_failures, connect_failures;
static bool deliver_late_stop;
static wifi_mode_t driver_mode;
static void wifi_lock(void) {}
static void wifi_unlock(void) {}
static void wifi_repeater_cancel_reconnect(void) { wifi_resume_reconnect_attempts = 0; }
static esp_err_t solar_os_wifi_repeater_disable(void) { return ESP_OK; }
static esp_err_t esp_wifi_disconnect(void) { return ESP_OK; }
static void wifi_clear_link_state(void) { wifi_connected = wifi_has_ip = false; }
static esp_err_t wifi_apply_nat(void) { return ESP_OK; }
static int wifi_power_save_mode(void) { return 0; }
static esp_err_t esp_wifi_set_ps(int value) { (void)value; return ESP_OK; }
static esp_err_t solar_os_network_path_set_ready(void *netif, bool ready)
{ (void)netif; assert(!ready); return ESP_OK; }
static void deliver_station_stop(void);
static esp_err_t esp_wifi_stop(void) { stops++; return ESP_OK; }
static esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{ driver_mode = mode; return ESP_OK; }
static esp_err_t esp_wifi_start(void) {
    starts++;
    if (deliver_late_stop) { deliver_late_stop = false; deliver_station_stop(); }
    if (start_failures) { start_failures--; return ESP_FAIL; }
    return ESP_OK;
}
static esp_err_t esp_wifi_connect(void) {
    connects++;
    assert(wifi_sta_enabled && wifi_started);
    if (connect_failures) { connect_failures--; return ESP_FAIL; }
    return ESP_OK;
}
static esp_err_t esp_wifi_set_channel(unsigned channel, int secondary)
{ assert(channel == 6 && secondary == 0); channels++; return ESP_OK; }
'''
        harness += production_functions(source, ("wifi_set_started_state",))
        harness += "\nstatic void deliver_station_stop(void) {\n" + stop_event + "}\n"
        harness += production_functions(source, (
            "wifi_desired_mode", "wifi_apply_mode", "solar_os_wifi_prepare_sleep",
            "solar_os_wifi_resume", "solar_os_wifi_stop"))
        harness += r'''
static void connected(void) {
    wifi_started = wifi_sta_enabled = wifi_connected = wifi_has_ip = true;
    wifi_ap_enabled = wifi_suspended = false;
    wifi_state = SOLAR_OS_WIFI_STATE_CONNECTING;
}
int main(void) {
    for (unsigned cycle = 0; cycle < 3; cycle++) {
        connected();
        assert(solar_os_wifi_prepare_sleep() == ESP_OK);
        assert(wifi_suspended && !wifi_started && wifi_sta_enabled);
        const unsigned before = stops;
        assert(solar_os_wifi_prepare_sleep() == ESP_OK && stops == before);
        /* STOP queued before sleep, delivered while wake starts the driver. */
        deliver_late_stop = true;
        assert(solar_os_wifi_resume() == ESP_OK);
        assert(wifi_sta_enabled && wifi_started && !wifi_suspended);
        assert(driver_mode == WIFI_MODE_STA && wifi_state == SOLAR_OS_WIFI_STATE_CONNECTING);
        /* Even a STOP delivered after resume cannot erase requested STA mode. */
        deliver_station_stop();
        assert(wifi_sta_enabled);
    }
    connected();
    assert(solar_os_wifi_prepare_sleep() == ESP_OK);
    start_failures = 1;
    assert(solar_os_wifi_resume() == ESP_FAIL && wifi_suspended);
    assert(solar_os_wifi_resume() == ESP_OK && !wifi_suspended && wifi_started);
    connected();
    assert(solar_os_wifi_prepare_sleep() == ESP_OK);
    connect_failures = 1;
    assert(solar_os_wifi_resume() == ESP_FAIL && wifi_suspended);
    assert(solar_os_wifi_resume() == ESP_OK && !wifi_suspended && wifi_sta_enabled);

    /* A station which was intentionally idle stays idle after wake. */
    connected(); wifi_connected = wifi_has_ip = false; wifi_state = SOLAR_OS_WIFI_STATE_IDLE;
    unsigned before = connects;
    assert(solar_os_wifi_prepare_sleep() == ESP_OK);
    assert(solar_os_wifi_resume() == ESP_OK && connects == before && wifi_started);
    /* An AP-only radio resumes without enabling station mode. */
    wifi_sta_enabled = false; wifi_ap_enabled = true;
    assert(solar_os_wifi_prepare_sleep() == ESP_OK);
    deliver_late_stop = true;
    assert(solar_os_wifi_resume() == ESP_OK);
    assert(driver_mode == WIFI_MODE_AP && wifi_ap_enabled && !wifi_sta_enabled);
    /* Connectionless owners retain their channel. */
    wifi_ap_enabled = false; wifi_connectionless_active = true;
    wifi_connectionless_channel = 6;
    assert(solar_os_wifi_prepare_sleep() == ESP_OK);
    assert(solar_os_wifi_resume() == ESP_OK && channels == 1);
    /* An off radio stays off, including repeated resumes. */
    wifi_connectionless_active = false; wifi_started = false;
    before = starts;
    assert(solar_os_wifi_prepare_sleep() == ESP_OK);
    assert(solar_os_wifi_resume() == ESP_OK && !wifi_suspended && !wifi_started);
    assert(solar_os_wifi_resume() == ESP_OK && starts == before);
    /* Explicit stop clears intent even if a driver STOP already arrived. */
    connected();
    deliver_station_stop();
    assert(!wifi_started && wifi_sta_enabled);
    assert(solar_os_wifi_stop() == ESP_OK && !wifi_sta_enabled && !wifi_ap_enabled);
    connected();
    assert(solar_os_wifi_prepare_sleep() == ESP_OK);
    start_failures = 1;
    assert(solar_os_wifi_resume() == ESP_FAIL && wifi_suspended);
    assert(solar_os_wifi_stop() == ESP_OK && !wifi_suspended);
    before = starts;
    assert(solar_os_wifi_resume() == ESP_OK && starts == before && !wifi_sta_enabled);
}
'''
        run_c(harness)


if __name__ == "__main__":
    unittest.main()
