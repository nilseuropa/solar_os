from pathlib import Path
import subprocess
import tempfile
import unittest
import test_tcp_stream as tcp_support

ROOT = Path(__file__).resolve().parents[1]


class NativeTcpStreamTest(unittest.TestCase):
    def test_real_loopback_registry_port_and_stream_through_the_abi(self):
        tcp_support.TcpStreamTest().run_fixture('native_tcp_stream_test.c', ('solar_os_native_tcp_stream.c',))

    def test_absent_without_socket_or_backend_dependencies(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            source = stage / 'absent.c'
            source.write_text('#include <assert.h>\n#include "solar_os_native_tcp_stream.h"\n'
                'int main(void){assert(!solar_os_native_tcp_stream_get_service("stream.tcp",1,8));return 0;}\n')
            binary = stage / 'absent'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-DSOLAR_OS_PACKAGE_SERVICE_TCP_STREAM=0', '-I' + str(ROOT / 'tests/host'),
                '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'src/services'), str(source),
                str(ROOT / 'src/services/solar_os_native_tcp_stream.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
