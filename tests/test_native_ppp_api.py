from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions


ROOT = Path(__file__).resolve().parents[1]


class NativePppApiTest(unittest.TestCase):
    def test_push_transport_route_io_and_retryable_teardown(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = (ROOT / 'src/services/solar_os_ppp.c').read_text()
            (work / 'ppp_io.inc').write_text(production_functions(source, (
                'ppp_io_acquire', 'ppp_io_release', 'ppp_io_busy', 'ppp_busy_locked',
                'ppp_connected_locked', 'ppp_set_session_locked', 'ppp_transmit', 'solar_os_ppp_receive',
            )))
            binary = work / 'ppp'
            subprocess.run([
                'cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
                '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-DSOLAR_OS_PACKAGE_SERVICE_PPP=1', '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', directory, '-I', str(ROOT / 'tests/host/native_abi_stubs'),
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'),
                '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_ppp_api_test.c'),
                str(ROOT / 'src/services/solar_os_native_ppp.c'),
                str(ROOT / 'src/services/solar_os_native_handles.c'), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
