from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeDeviceApiTest(unittest.TestCase):
    def run_configuration(self, enabled):
        with tempfile.TemporaryDirectory() as tmp:
            # Deliberately different SDK enum numbers: the ABI must translate.
            names = ['UNDEFINED', 'ALL', 'EXT0', 'EXT1', 'TIMER', 'TOUCHPAD', 'ULP', 'GPIO',
                     'UART', 'UART1', 'UART2', 'WIFI', 'COCPU', 'COCPU_TRAP_TRIG', 'BT',
                     'VAD', 'VBAT_UNDER_VOLT', 'USB']
            Path(tmp, 'esp_sleep.h').write_text('#pragma once\ntypedef enum {\n' +
                ',\n'.join(f'ESP_SLEEP_WAKEUP_{name} = {1000 + i * 3}' for i, name in enumerate(names)) +
                '\n} esp_sleep_wakeup_cause_t;\n')
            binary = Path(tmp) / 'device'
            command = ['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie']
            command += [f'-DSOLAR_OS_PACKAGE_{package}=1' for package in enabled]
            command += ['-I', tmp, '-I', str(ROOT / 'tests/host/native_abi_stubs'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_device_api_test.c'), str(ROOT / 'src/services/solar_os_native_device.c'),
                str(ROOT / 'src/services/solar_os_native_device_providers.c'), str(ROOT / 'src/services/solar_os_native_handles.c')]
            for package, source in [('CORE_RUNTIME', 'shutdown'), ('SERVICE_CHARGER', 'charger'), ('SERVICE_HAPTIC', 'haptic')]:
                if package in enabled:
                    command += [str(ROOT / f'src/services/solar_os_{source}.c')]
            subprocess.run(command + ['-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_device_records_ranges_power_enums_and_shutdown_failure(self):
        self.run_configuration(['CORE_RUNTIME', 'SERVICE_GPIO', 'SERVICE_BATTERY', 'SERVICE_CHARGER', 'SERVICE_HAPTIC'])

    def test_no_optional_service_dependencies(self):
        self.run_configuration([])

    def test_battery_only_configuration(self):
        self.run_configuration(['SERVICE_BATTERY'])
