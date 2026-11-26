from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class NativeWireGuardTest(unittest.TestCase):
    def test_shared_operations_serialize_profile_import_and_start(self):
        services = ROOT / "src/services"
        host = ROOT / "tests/host"
        source = (services / "solar_os_wireguard.c").read_text()
        wrappers = production_functions(source, (
            "wireguard_controlled", "solar_os_wireguard_init", "solar_os_wireguard_import",
            "solar_os_wireguard_forget", "solar_os_wireguard_up", "solar_os_wireguard_down",
            "solar_os_wireguard_prepare_sleep", "solar_os_wireguard_resume", "solar_os_wireguard_get_status",
            "solar_os_wireguard_state_name", "solar_os_wireguard_policy_name",
        ))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            fixture = path / "wireguard.c"
            fixture.write_text(PRELUDE + wrappers + CHECKS)
            binary = path / "wireguard"
            subprocess.run([
                "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                "-pthread", "-fsanitize=address,undefined", "-no-pie",
                "-DSOLAR_OS_PACKAGE_SERVICE_WIREGUARD=1", f"-I{host / 'native_abi_stubs'}",
                f"-I{host}", f"-I{ROOT / 'include'}", f"-I{services}",
                str(fixture), str(services / "solar_os_native_wireguard.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=15)


PRELUDE = r"""
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "solar_os_native_wireguard.h"
#include "solar_os_sync.h"
#include "solar_os_wireguard.h"

static solar_os_lifecycle_t wireguard_control;
static atomic_bool importing, allow_import, starting, started;
static solar_os_wireguard_status_t configuration;

static void sleep_ms(unsigned ms)
{
    struct timespec delay = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}

static esp_err_t wireguard_init_controlled(void)
{ configuration.initialized = true; return ESP_OK; }

static esp_err_t wireguard_import_controlled(const char *path, char *detail, size_t size)
{
    assert(!strcmp(path, "/flash/camera-vpn.conf"));
    atomic_store(&importing, true);
    while (!atomic_load(&allow_import)) sleep_ms(1);
    configuration.configured = true;
    if (detail && size) snprintf(detail, size, "imported");
    return ESP_OK;
}

static esp_err_t wireguard_forget_controlled(void)
{
    if (configuration.desired_up) return ESP_ERR_INVALID_STATE;
    configuration.configured = false;
    return ESP_OK;
}

static esp_err_t wireguard_up_controlled(solar_os_wireguard_policy_t policy)
{
    assert(configuration.configured);
    configuration.policy = policy;
    configuration.desired_up = true;
    configuration.state = SOLAR_OS_WIREGUARD_STATE_WAIT_UPLINK;
    configuration.last_error = ESP_ERR_TIMEOUT;
    atomic_store(&started, true);
    return ESP_OK;
}
static esp_err_t wireguard_down_controlled(void)
{ configuration.desired_up = false; return ESP_OK; }
static esp_err_t wireguard_prepare_sleep_controlled(void) { return ESP_OK; }
static esp_err_t wireguard_resume_controlled(void) { return ESP_OK; }
static void wireguard_status_controlled(solar_os_wireguard_status_t *out) { *out = configuration; }
"""

CHECKS = r"""
static const solar_os_native_wireguard_api_v1_t *api;

static void *native_import(void *unused)
{
    (void)unused;
    char detail[12];
    assert(api->import_profile("/flash/camera-vpn.conf", detail, sizeof(detail)) == SOLAR_OS_NATIVE_OK);
    assert(!strcmp(detail, "imported"));
    return NULL;
}

static void *builtin_start(void *unused)
{
    (void)unused;
    atomic_store(&starting, true);
    assert(solar_os_wireguard_up(SOLAR_OS_WIREGUARD_POLICY_FAIL_CLOSED) == ESP_OK);
    return NULL;
}

int main(void)
{
    assert(solar_os_wireguard_init() == ESP_OK);
    api = solar_os_native_wireguard_get_service(SOLAR_OS_NATIVE_WIREGUARD_SERVICE,
        SOLAR_OS_NATIVE_WIREGUARD_ABI, sizeof(*api));
    assert(api && !solar_os_native_wireguard_get_service(SOLAR_OS_NATIVE_WIREGUARD_SERVICE, 2, sizeof(*api)));
    assert(api->import_profile(NULL, NULL, 0) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    pthread_t importing_task, start_task;
    assert(!pthread_create(&importing_task, NULL, native_import, NULL));
    while (!atomic_load(&importing)) sleep_ms(1);
    assert(!pthread_create(&start_task, NULL, builtin_start, NULL));
    while (!atomic_load(&starting)) sleep_ms(1);
    sleep_ms(20);
    assert(!atomic_load(&started));
    atomic_store(&allow_import, true);
    pthread_join(importing_task, NULL);
    pthread_join(start_task, NULL);
    assert(atomic_load(&started));

    solar_os_wireguard_status_t status;
    assert(api->status(&status) == SOLAR_OS_NATIVE_OK && status.configured && status.desired_up);
    assert(!strcmp(api->state_name(status.state), "waiting for uplink"));
    assert(!strcmp(api->policy_name(status.policy), "fail-closed"));
    assert(status.last_error == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(api->forget() == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(api->down() == SOLAR_OS_NATIVE_OK && api->forget() == SOLAR_OS_NATIVE_OK);
    assert(api->status(&status) == SOLAR_OS_NATIVE_OK && !status.configured && !status.desired_up);
    puts("WireGuard ABI: serial profile changes across native and built-in calls, state and errors passed");
    return 0;
}
"""
