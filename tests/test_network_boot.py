from pathlib import Path
import unittest

from tests.test_foreground_presenter import production_functions
from tests.test_shutdown_policy import run_c


ROOT = Path(__file__).resolve().parents[1]


class NetworkBootTest(unittest.TestCase):
    def test_stack_is_ready_without_wifi_and_initialization_errors_stop_boot(self):
        network = (ROOT / "src/services/solar_os_network.c").read_text()
        boot = (ROOT / "src/services/solar_os_boot_services.c").read_text()
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
#include "esp_err.h"
#define SOLAR_OS_PACKAGE_SERVICE_NETWORK 1
#define SOLAR_OS_PACKAGE_SERVICE_WIFI 1
#define ESP_ERR_NOT_ALLOWED 0x10D
#define SOLAR_OS_BOARD_CAP_CDC 1
#define SOLAR_OS_BOARD_CAP_WIFI 2
#define SOLAR_OS_LOGW(...) ((void)0)
#define SOLAR_OS_LOGI(...) ((void)0)
#define ESP_ERROR_CHECK(expr) do { \
    boot_error = (expr); \
    if (boot_error != ESP_OK) longjmp(boot_failure, 1); \
} while (0)
static jmp_buf boot_failure;
static esp_err_t boot_error, stack_error, loop_error;
static bool stack_ready, loop_ready, has_wifi, wifi_allowed;
static unsigned services_started, wifi_attempts, radio_inits;
static esp_err_t esp_netif_init(void) {
    if (stack_error != ESP_OK) return stack_error;
    stack_ready = true;
    return ESP_OK;
}
static esp_err_t esp_event_loop_create_default(void) {
    assert(stack_ready);
    if (loop_error != ESP_OK) return loop_error;
    if (loop_ready) return ESP_ERR_INVALID_STATE;
    loop_ready = true;
    return ESP_OK;
}
static esp_err_t ready_service(void) {
    assert(stack_ready && loop_ready);
    services_started++;
    return ESP_OK;
}
#define solar_os_stream_init ready_service
#define solar_os_port_init ready_service
#define solar_os_cdc_init ready_service
#define solar_os_power_init ready_service
#define solar_os_storage_init ready_service
#define solar_os_identity_init ready_service
static bool solar_os_board_has(int cap) {
    return cap == SOLAR_OS_BOARD_CAP_WIFI && has_wifi;
}
static void solar_os_power_note_activity(uint32_t now) { assert(now == 123); }
static esp_err_t solar_os_wifi_init(void) {
    wifi_attempts++;
    if (!wifi_allowed) return ESP_ERR_NOT_ALLOWED;
    assert(esp_netif_init() == ESP_OK);
    radio_inits++;
    return ESP_OK;
}
/* Model the lwIP precondition that asserted in tcpip_send_msg_wait_sem. */
static void startup_socket_request(void) { assert(stack_ready); }
'''
        harness += production_functions(network, ("solar_os_network_init",))
        harness += production_functions(boot, ("solar_os_boot_services_init",))
        harness += r'''
int main(void) {
    /* A disabled Wi-Fi driver must not leave startup jobs without lwIP. */
    has_wifi = true;
    solar_os_boot_services_init(123);
    assert(wifi_attempts == 1 && radio_inits == 0 && services_started > 0);
    startup_socket_request();
    /* Repeated initialization accepts the existing default event loop. */
    assert(solar_os_network_init() == ESP_OK);

    stack_ready = loop_ready = false;
    wifi_allowed = true;
    solar_os_boot_services_init(123);
    assert(wifi_attempts == 2 && radio_inits == 1);
    startup_socket_request();

    /* Non-Wi-Fi transports also get the shared stack on boards without Wi-Fi. */
    stack_ready = loop_ready = has_wifi = false;
    solar_os_boot_services_init(123);
    assert(wifi_attempts == 2 && radio_inits == 1);
    startup_socket_request();

    stack_ready = loop_ready = false;
    services_started = 0;
    stack_error = ESP_ERR_NO_MEM;
    if (setjmp(boot_failure) == 0) {
        solar_os_boot_services_init(123);
        assert(false);
    }
    assert(boot_error == ESP_ERR_NO_MEM && !stack_ready && !loop_ready);
    assert(services_started == 0);

    stack_error = ESP_OK;
    loop_error = ESP_FAIL;
    if (setjmp(boot_failure) == 0) {
        solar_os_boot_services_init(123);
        assert(false);
    }
    assert(boot_error == ESP_FAIL && !loop_ready && services_started == 0);
}
'''
        run_c(harness)


if __name__ == "__main__":
    unittest.main()
