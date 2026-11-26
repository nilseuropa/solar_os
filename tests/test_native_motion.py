from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeMotionTest(unittest.TestCase):
    def check_motion(self, enabled):
        host = ROOT / 'tests/host'
        services = ROOT / 'src/services'
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'motion'
            command = ['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
                '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                f'-DSOLAR_OS_PACKAGE_SERVICE_MOTOR={int(enabled)}',
                f'-DSOLAR_OS_PACKAGE_SERVICE_ENCODER={int(enabled)}',
                '-DSOLAR_OS_PACKAGE_SERVICE_SERVO=0',
                '-include', str(host / 'compat.h'), '-I', str(host / 'pwm_stubs'),
                '-I', str(host), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'),
                '-I', str(services), str(host / 'native_motion_test.c'),
                str(services / 'solar_os_native_motion.c')]
            if enabled:
                command += [str(services / f'solar_os_{name}.c') for name in
                            ('motor', 'encoder', 'stream', 'parameters')]
                command += [str(host / 'motion_endpoint_clock.c')]
            subprocess.run([*command, '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_shared_device_operations_claims_and_calibration(self):
        self.check_motion(True)

    def test_absent_device_methods(self):
        self.check_motion(False)
