from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class NativeGfxTest(unittest.TestCase):
    def test_real_renderer_pixels_geometry_fonts_icons_and_buffer_lifetimes(self):
        component = ROOT / 'components/u8g2'
        cmake = (component / 'CMakeLists.txt').read_text().split('\n)', 1)[0]
        sources = [component / name for name in re.findall(r'"([^"]+\.c)"', cmake)]
        sources += sorted((ROOT / 'fonts/build/u8g2').glob('u8g2_font_solar_os_default_*_tf.c'))
        sources += [ROOT / 'src/services' / name for name in
            ('solar_os_display_geometry.c', 'solar_os_gfx_icons.c', 'solar_os_native_gfx.c',
             'solar_os_gfx_widgets.c', 'solar_os_native_ui.c', 'solar_os_tui_widgets.c',
             'solar_os_native_media_widgets.c', 'solar_os_media_widgets.c')]
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            decoder = stage / 'utf8.c'
            decoder.write_text('#include <stdint.h>\n#include <stddef.h>\n' + production_functions(
                (ROOT / 'src/services/solar_os_fonts.c').read_text(), ('solar_os_utf8_next',)))
            pack = (ROOT / 'fonts/packs/cyrillic.sfp').read_bytes()
            ranges, count = struct.unpack_from('<HH', pack, 4)
            faces = [struct.unpack_from('<BB2xII', pack, 24 + ranges * 8 + i * 12)
                     for i in range(count)]
            _, _, offset, size = next(face for face in faces if face[0] == 0 and face[1] == 14)
            font = stage / 'pack.c'
            font.write_text('#include <stdint.h>\nconst uint8_t test_pack_face[] = {' +
                ','.join(map(str, pack[offset:offset + size])) + '};\n')
            binary = stage / 'gfx'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1',
                '-DSOLAR_OS_PACKAGE_SERVICE_MEDIA_WIDGETS=1',
                '-O1', '-g', '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'),
                '-I', str(ROOT / 'src/services'), '-I', str(component / 'src/clib'),
                '-I', str(ROOT / 'src/shell'),
                str(ROOT / 'tests/host/native_gfx_test.c'), str(decoder), str(font), *map(str, sources),
                '-Wl,--gc-sections', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_service_is_absent_without_core_and_sdk_keeps_backends_private(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / 'absent.c').write_text('#include <assert.h>\n#include "solar_os_native_gfx.h"\n'
                '#include "solar_os_native_ui.h"\n'
                '#include "solar_os_native_media_widgets.h"\n'
                'int main(void){assert(!solar_os_native_gfx_get_service("gfx",1,8));'
                'assert(!solar_os_native_media_widgets_get_service("ui.media",1,8));'
                'assert(!solar_os_native_ui_get_service("ui",1,8));return 0;}\n')
            subprocess.run(['cc', '-std=c11', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=0', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src/services'),
                str(stage / 'absent.c'), str(ROOT / 'src/services/solar_os_native_gfx.c'),
                str(ROOT / 'src/services/solar_os_native_ui.c'),
                str(ROOT / 'src/services/solar_os_native_media_widgets.c'), '-o', str(stage / 'absent')], check=True)
            subprocess.run([str(stage / 'absent')], check=True)
        public = (ROOT / 'include/solar_os_native_gfx_abi.h').read_text()
        self.assertNotIn('u8g2', public)
        self.assertNotIn('solar_os_display_surface_t', public)
