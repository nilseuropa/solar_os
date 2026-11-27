from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

class NativeNetworkApiTest(unittest.TestCase):
    def test_channels_metadata_and_cross_task_cancellation(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/'net'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-pthread',
                '-fsanitize=address,undefined','-fno-pie','-no-pie',
                '-DSOLAR_OS_PACKAGE_SERVICE_NET=1','-DSOLAR_OS_PACKAGE_SERVICE_SCRIPT_NET=1',
                '-I',str(ROOT/'tests/host/native_abi_stubs'),'-I',str(ROOT/'tests/host'),
                '-I',str(ROOT/'include'),'-I',str(ROOT/'src'),'-I',str(ROOT/'src/services'),
                str(ROOT/'tests/host/native_network_api_test.c'),
                str(ROOT/'src/services/solar_os_native_handles.c'),
                str(ROOT/'src/services/solar_os_native_network.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=20)

    def test_disabled_network_services_have_no_backend_dependencies(self):
        code='#include <assert.h>\n#include "solar_os_native_network.h"\n#include "solar_os_native_http.h"\nint main(void){assert(!solar_os_native_network_get_service("net",1,8));assert(!solar_os_native_http_get_service("http",1,8));}\n'
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/'disabled'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-x','c',
                '-I',str(ROOT/'tests/host'),'-I',str(ROOT/'include'),
                '-I',str(ROOT/'src'),'-I',str(ROOT/'src/services'),'-',
                str(ROOT/'src/services/solar_os_native_network.c'),
                str(ROOT/'src/services/solar_os_native_http.c'),'-o',str(binary)],
                input=code,text=True,check=True)
            subprocess.run([str(binary)],check=True)
