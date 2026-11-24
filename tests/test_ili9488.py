from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class Ili9488Test(unittest.TestCase):
    def test_production_rgb666_bands_and_display_context(self):
        source = (ROOT / 'src/drivers/tft_ili9341.c').read_text()
        start = source.index('typedef void (*ili9341_line_renderer_t)')
        end = source.index('static esp_err_t ili9341_transmit_rendered_lines', start)
        functions = production_functions(source, (
            'ili9488_expand_rgb565', 'ili9341_wire_row_bytes', 'ili9341_fill_line',
            'ili9341_transmit_rendered_lines', 'ili9341_from_u8x8',
        ))
        fixture = (ROOT / 'tests/host/ili9488_test.inc').read_text()
        code = fixture.replace('/* FUNCTIONS */', source[start:end] + functions)
        with tempfile.TemporaryDirectory() as temp:
            path, binary = Path(temp, 'ili9488.c'), Path(temp, 'ili9488')
            path.write_text(code)
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I' + str(ROOT / 'tests/host/epd_stubs'),
                '-I' + str(ROOT / 'tests/host'), '-I' + str(ROOT / 'src/drivers'),
                '-I' + str(ROOT / 'src/services'),
                '-I' + str(ROOT / 'components/u8g2/src/clib'),
                str(path), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
