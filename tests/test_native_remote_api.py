from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions
ROOT = Path(__file__).resolve().parents[1]


class NativeRemoteApiTest(unittest.TestCase):
    def test_ssh_transfer_values_handles_and_sftp_callback_lifetimes(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'remote'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-DSOLAR_OS_PACKAGE_SERVICE_SSH=1', '-DSOLAR_OS_PACKAGE_SERVICE_SFTP=1', '-DSOLAR_OS_PACKAGE_SERVICE_SFTPSYNC=1',
                '-I', str(ROOT / 'tests/host/native_abi_stubs'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_remote_api_test.c'),
                str(ROOT / 'src/services/solar_os_native_handles.c'),
                str(ROOT / 'src/services/solar_os_native_remote.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_strict_ssh_scp_and_sync_stop_preserves_worker_on_timeout(self):
        for name in ('ssh', 'scp', 'sftpsync'):
            function = 'solar_os_' + name + '_stop_wait'
            source = (ROOT / ('src/services/solar_os_' + name + '.c')).read_text()
            harness = '''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct { void *task; bool task_done, stop_requested, detached; } solar_os_NAME_session_t;
static unsigned destroyed;
static bool solar_os_task_wait_done(void *task, const volatile bool *done, uint32_t timeout)
{ assert(task && timeout <= 60000); return *done; }
static void NAME_session_destroy(solar_os_NAME_session_t *session) { assert(session->task_done); ++destroyed; }
'''.replace('NAME', name) + production_functions(source, (function,)) + '''
int main(void) {
    solar_os_NAME_session_t state = {.task = (void *)1};
    assert(solar_os_NAME_stop_wait(&state, 7) == ESP_ERR_TIMEOUT);
    assert(state.stop_requested && !state.detached && !destroyed);
    assert(solar_os_NAME_stop_wait(&state, 60001) == ESP_ERR_INVALID_ARG);
    state.task_done = true; assert(!solar_os_NAME_stop_wait(&state, 0)); assert(destroyed == 1);
    return 0;
}
'''.replace('NAME', name)
            with self.subTest(service=name), tempfile.TemporaryDirectory() as tmp:
                binary = Path(tmp) / name
                subprocess.run(['cc', '-x', 'c', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-I', str(ROOT / 'tests/host'),
                    '-o', str(binary), '-'], input=harness, text=True, check=True)
                subprocess.run([str(binary)], check=True)
