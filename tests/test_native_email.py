from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeEmailTest(unittest.TestCase):
    def test_shared_store_configuration_failure_and_copied_messages(self):
        services = ROOT / 'src/services'
        host = ROOT / 'tests/host'
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory, 'email')
            for enabled in (0, 1):
                with self.subTest(email_available=enabled):
                    sources = [str(services / name) for name in ('solar_os_email.c', 'solar_os_email_sync.c')] if enabled else []
                    subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
                        '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-DESP_ERR_NOT_ALLOWED=0x10D',
                        f'-DSOLAR_OS_PACKAGE_SERVICE_MAIL={enabled}',
                        '-include', str(host / 'compat.h'), '-I', str(host / 'native_abi_stubs'),
                        '-I', str(host), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'),
                        '-I', str(services), str(host / 'native_email_test.c'),
                        str(services / 'solar_os_native_inbox.c'), *sources, '-o', str(binary)], check=True)
                    subprocess.run([str(binary)], check=True, timeout=20)
