from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions
from test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class NativeBusApiTest(unittest.TestCase):
    def test_bus_values_protocol_leases_and_autobaud_cleanup(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'bus'
            labels = Path(tmp) / 'labels.c'
            source = (ROOT / 'src/services/solar_os_buses.c').read_text()
            labels.write_text('#include "solar_os_buses.h"\n' + production_functions(source, (
                'solar_os_bus_protocol_name', 'solar_os_bus_origin_name', 'solar_os_bus_sharing_name',
            )))
            for available in (0, 1):
                subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-DSOLAR_OS_PACKAGE_SERVICE_RESOURCES=1',
                    '-DSOLAR_OS_PACKAGE_SERVICE_EXPANSION=1',
                    f'-DSOLAR_OS_PACKAGE_SERVICE_UART={available}', f'-DSOLAR_OS_PACKAGE_SERVICE_SPI={available}',
                    f'-DSOLAR_OS_PACKAGE_SERVICE_ONEWIRE={available}', f'-DSOLAR_OS_PACKAGE_SERVICE_PS2={available}',
                    '-DESP_ERR_NOT_ALLOWED=0x10d', '-include', str(ROOT / 'tests/host/compat.h'),
                    '-I', str(ROOT / 'tests/host/native_abi_stubs'), '-I', str(ROOT / 'tests/host/expansion_stubs'),
                    '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                    str(ROOT / 'tests/host/native_bus_api_test.c'),
                    str(labels),
                    str(ROOT / 'src/services/solar_os_native_handles.c'), str(ROOT / 'src/services/solar_os_native_bus.c'),
                    *([str(ROOT / 'src/services' / name) for name in
                       ('solar_os_spi.c', 'solar_os_ps2_keyboard.c', 'solar_os_ps2_mouse.c')] if available else []),
                    '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True, timeout=20)

    def test_uart_mode_persists_only_when_the_owner_can_change_it(self):
        source = ROOT / 'src/services'
        uart = production_functions((source / 'solar_os_uart.c').read_text(), (
            'uart_require_available', 'uart_bus_set_mode', 'solar_os_uart_bus_set_mode',
            'solar_os_uart_bus_set_mode_owned', 'solar_os_uart_set_mode',
            'solar_os_uart_copy_bus_status', 'solar_os_uart_get_bus_status'))
        run_c(r'''
#include <assert.h>
#include <string.h>
#include "solar_os_uart.h"
#define ESP_RETURN_ON_ERROR(call, tag, message) do { \
    esp_err_t error = (call); if (error != ESP_OK) return error; \
} while (0)
typedef int uart_port_t;
typedef struct {
    char name[SOLAR_OS_BUS_NAME_MAX];
    bool attached, initialized, persistent;
    solar_os_uart_mode_t mode;
    solar_os_bus_uart_config_t config;
} solar_os_uart_instance_t;
typedef struct { solar_os_uart_instance_t *instance; } solar_os_uart_ref_t;
static solar_os_uart_instance_t endpoint = {.name = "uart1", .persistent = true,
    .attached = true, .initialized = true, .config = {.port = 1, .rx_pin = 2, .tx_pin = 3, .baud_rate = 115200}};
static bool pinned, claimed = true, fail_save, busy;
static int saved;
esp_err_t solar_os_uart_init(void) { return ESP_OK; }
static esp_err_t uart_pin_name_timeout(const char *name, int wait, solar_os_uart_ref_t *out) {
    assert(!pinned);
    if (strcmp(name, endpoint.name)) return ESP_ERR_NOT_FOUND;
    if (busy && !wait) return ESP_ERR_TIMEOUT;
    pinned = true;
    out->instance = &endpoint;
    return ESP_OK;
}
static esp_err_t uart_pin_name(const char *name, solar_os_uart_ref_t *out) {
    return uart_pin_name_timeout(name, 1, out);
}
static void uart_unpin(solar_os_uart_ref_t *ref) {
    assert(pinned && ref->instance == &endpoint);
    pinned = false;
}
static esp_err_t uart_save_config(const solar_os_uart_instance_t *instance) {
    assert(pinned && instance == &endpoint);
    if (fail_save) return ESP_FAIL;
    ++saved;
    return ESP_OK;
}
static esp_err_t uart_port_get_rx_buffered(uart_port_t port, size_t *bytes) {
    assert(pinned && port == 1);
    *bytes = 42;
    return ESP_OK;
}
esp_err_t solar_os_port_get_info(const char *name, solar_os_port_info_t *info) {
    assert(pinned && !strcmp(name, endpoint.name));
    *info = (solar_os_port_info_t){.claimed = claimed, .owner = "lease"};
    return ESP_OK;
}
static size_t strlcpy(char *to, const char *from, size_t capacity) {
    size_t bytes = strlen(from); assert(bytes < capacity);
    memcpy(to, from, bytes + 1);
    return bytes;
}
''' + uart + r'''
int main(void) {
    const solar_os_uart_mode_t line = SOLAR_OS_UART_MODE_LINE;
    assert(solar_os_uart_bus_set_mode("uart1", line) == ESP_ERR_INVALID_STATE);
    assert(solar_os_uart_bus_set_mode_owned("uart1", line, "other") == ESP_ERR_INVALID_STATE);
    assert(solar_os_uart_bus_set_mode_owned("uart1", line, "") == ESP_ERR_INVALID_ARG);
    assert(!saved && endpoint.mode == SOLAR_OS_UART_MODE_RAW);
    fail_save = true;
    assert(solar_os_uart_bus_set_mode_owned("uart1", line, "lease") == ESP_FAIL);
    assert(!saved && endpoint.mode == SOLAR_OS_UART_MODE_RAW && !pinned);
    fail_save = false;
    assert(!solar_os_uart_bus_set_mode_owned("uart1", line, "lease") && saved == 1);
    solar_os_uart_status_t status;
    assert(!solar_os_uart_copy_bus_status("uart1", &status) && status.mode == line);
    assert(status.port_num == 1 && status.rx_buffered_valid && status.rx_buffered == 42);
    assert(status.port_claimed && !strcmp(status.port_owner, "lease"));
    assert(solar_os_uart_copy_bus_status("missing", &status) == ESP_ERR_NOT_FOUND);
    busy = true;
    assert(solar_os_uart_copy_bus_status("uart1", &status) == ESP_ERR_TIMEOUT);
    assert(!solar_os_uart_get_bus_status("uart1", &status));
    busy = claimed = false;
    assert(!solar_os_uart_bus_set_mode("uart1", SOLAR_OS_UART_MODE_RAW) && saved == 2);
    assert(solar_os_uart_get_bus_status("uart1", &status) && !status.port_claimed);
}
''', '-I', str(source), '-fsanitize=address,undefined', '-no-pie')
