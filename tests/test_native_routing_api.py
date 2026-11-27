from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions
ROOT = Path(__file__).resolve().parents[1]


class NativeRoutingApiTest(unittest.TestCase):
    def test_copied_status_dns_scan_and_radio_lease_ownership(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, 'wifi_release.inc').write_text(production_functions(
                (ROOT / 'src/services/solar_os_wifi.c').read_text(),
                ('solar_os_wifi_connectionless_release', 'solar_os_wifi_latency_release', 'solar_os_wifi_state_name')))
            Path(tmp, 'network_names.inc').write_text(production_functions(
                (ROOT / 'src/services/solar_os_network.c').read_text(),
                ('solar_os_network_interface_role_name', 'solar_os_network_interface_state_name')))
            binary = Path(tmp) / 'routing'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-include', str(ROOT / 'tests/host/compat.h'),
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-DSOLAR_OS_PACKAGE_SERVICE_NETWORK=1', '-DSOLAR_OS_PACKAGE_SERVICE_WIFI=1',
                '-I', tmp,
                '-I', str(ROOT / 'tests/host/native_abi_stubs'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_routing_api_test.c'),
                str(ROOT / 'src/services/solar_os_native_handles.c'),
                str(ROOT / 'src/services/solar_os_router.c'),
                str(ROOT / 'src/services/solar_os_native_router_provider.c'),
                str(ROOT / 'src/services/solar_os_native_routing.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
