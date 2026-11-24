from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class QemuDisplayTest(unittest.TestCase):
    def test_production_color_mono_rotation_and_padded_geometry(self):
        source = (ROOT / 'src/services/solar_os_qemu_rgb_display.c').read_text()
        declarations = source[source.index('#if SOLAR_OS_BOARD_HAS_DISPLAY'):
                              source.index('static const u8x8_display_info_t')]
        functions = production_functions(source, (
            'rgb565', 'present_draw_buffer', 'set_colors', 'present_raster',
            'present_mono_xbm', 'present_surface',
        ))
        fixture = (ROOT / 'tests/host/qemu_display_test.inc').read_text()
        with tempfile.TemporaryDirectory() as temp:
            path, binary = Path(temp, 'display.c'), Path(temp, 'display')
            path.write_text(fixture.replace('/* FUNCTIONS */', declarations + functions))
            for width, height in ((480, 320), (300, 400), (5, 9)):
                with self.subTest(width=width, height=height):
                    subprocess.run([
                        'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-DSOLAR_OS_BOARD_HAS_DISPLAY=1',
                        f'-DSOLAR_OS_BOARD_DISPLAY_WIDTH={width}',
                        f'-DSOLAR_OS_BOARD_DISPLAY_HEIGHT={height}',
                        '-I' + str(ROOT / 'tests/host'),
                        '-I' + str(ROOT / 'src/services'),
                        '-I' + str(ROOT / 'components/u8g2/src/clib'),
                        str(path), '-o', str(binary),
                    ], check=True)
                    subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
