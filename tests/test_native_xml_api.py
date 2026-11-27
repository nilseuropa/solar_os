from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class NativeXmlApiTest(unittest.TestCase):
    def test_real_parser_events_attributes_and_synchronous_callback_lifetime(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'xml'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-DSOLAR_OS_PACKAGE_SERVICE_DOCUMENT=1',
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'),
                '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_xml_api_test.c'), str(ROOT / 'src/services/solar_os_xml.c'),
                str(ROOT / 'src/services/solar_os_native_xml.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
