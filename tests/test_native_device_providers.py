from pathlib import Path
import subprocess
import tempfile
import unittest
from test_native_libraries import write_runtime_support
ROOT = Path(__file__).resolve().parents[1]


class NativeDeviceProvidersTest(unittest.TestCase):
    def compile(self, tmp, fixture, enabled):
        binary = Path(tmp) / 'providers'
        command = ['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread',
            '-fsanitize=address,undefined', '-fno-pie', '-no-pie']
        command += [f'-DSOLAR_OS_PACKAGE_{package}=1' for package in enabled]
        command += ['-I', tmp, '-I', str(ROOT / 'tests/host/native_abi_stubs'), '-I', str(ROOT / 'tests/host'),
            '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
            str(fixture), str(ROOT / 'src/services/solar_os_native_device_providers.c'),
            str(ROOT / 'src/services/solar_os_native_handles.c')]
        for package, source in [('CORE_RUNTIME', 'backlight'), ('SERVICE_CHARGER', 'charger'), ('SERVICE_HAPTIC', 'haptic')]:
            if package in enabled:
                command += [str(ROOT / f'src/services/solar_os_{source}.c')]
        subprocess.run(command + ['-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=20)

    def test_callbacks_code_leases_busy_close_and_replacement_ownership(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.compile(tmp, ROOT / 'tests/host/native_device_providers_test.c',
                ['CORE_RUNTIME', 'SERVICE_CHARGER', 'SERVICE_HAPTIC'])

    def test_provider_holds_real_loader_lease_until_unregistered(self):
        fixture = (ROOT / 'tests/host/native_libraries_test.c').read_text()
        fixture = fixture.replace('int main(void)', 'int original_library_checks(void)')
        fixture = fixture.replace('../../src/services/solar_os_native_runtime.c',
            str(ROOT / 'src/services/solar_os_native_runtime.c'))
        fixture += r'''
#include "solar_os_native_device_providers.h"
#include "solar_os_haptic.h"
static int provider_play(void *user, uint32_t value) { assert(user == &images && value == 2 && images == 1); return 0; }
static int provider_stop(void *user) { assert(user == &images && images == 1); return 0; }
int main(void)
{
    uint32_t lease; const void *provider_api; acquire(&lease, &provider_api);
    const solar_os_native_haptic_provider_v1_t provider = {.name = "motor", .driver = "library", .effect_count = 3,
        .library_lease = lease, .user = &images, .play_effect = provider_play, .stop = provider_stop};
    solar_os_native_handle_t handle; assert(!solar_os_native_haptic_provider_register(&provider, &handle));
    assert(!solar_os_native_library_release(lease));
    assert(solar_os_native_library_unload("example") == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && images == 1);
    assert(!solar_os_haptic_play_effect("motor", 2));
    assert(!solar_os_native_haptic_provider_close(handle) && !solar_os_haptic_count());
    assert(!solar_os_native_library_unload("example") && !images && !allocations && !native_library_leases);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            write_runtime_support(Path(tmp))
            path = Path(tmp) / 'provider_library.c'; path.write_text(fixture)
            self.compile(tmp, path, ['SERVICE_HAPTIC'])
