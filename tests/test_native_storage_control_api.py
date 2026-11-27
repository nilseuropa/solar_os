from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions
ROOT=Path(__file__).resolve().parents[1]


class NativeStorageControlApiTest(unittest.TestCase):
    def test_raw_image_close_consumption_metadata_and_settings_results(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp,'image_close.inc').write_text(production_functions(
                (ROOT/'src/services/solar_os_disk_image.c').read_text(),
                ('sync_image','close_locked','solar_os_disk_image_close')))
            binary=Path(tmp)/'storage'
            subprocess.run(['cc','-std=c11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-pthread',
                '-fsanitize=address,undefined','-fno-pie','-no-pie','-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1','-I',tmp,
                '-I',str(ROOT/'tests/host/native_abi_stubs'),'-I',str(ROOT/'tests/host'),'-I',str(ROOT/'include'),
                '-I',str(ROOT/'src'),'-I',str(ROOT/'src/services'),str(ROOT/'tests/host/native_storage_control_api_test.c'),
                str(ROOT/'src/services/solar_os_native_handles.c'),str(ROOT/'src/services/solar_os_native_storage_control.c'),
                '-Wl,--wrap=fsync','-o',str(binary)],check=True)
            subprocess.run([str(binary),str(Path(tmp)/'disk.img')],check=True,timeout=20)
