from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class SynthPointerTest(unittest.TestCase):
    def test_controls_piano_drag_release_hold_and_wave_editing(self):
        source = (ROOT / 'src/apps/solar_os_synth_app.c').read_text()
        declarations = source[source.index('typedef enum {'):
                              source.index('typedef struct {\n  uint32_t magic;')]
        functions = production_functions(source, (
            'synth_add_touch_target', 'synth_touch_target_at',
            'synth_bottom_height', 'synth_tab_button_rect', 'synth_tab_at', 'synth_piano_note_at',
            'synth_note_frequency', 'synth_find_held', 'synth_allocate_held',
            'synth_note_on', 'synth_release_held', 'synth_note_off',
            'synth_pointer_key', 'synth_pointer_release_note', 'synth_pointer_cancel',
            'synth_toggle_note', 'synth_set_hold_mode', 'synth_select_tab',
            'synth_touch_wave', 'synth_touch_select', 'synth_touch_adjust', 'synth_pointer_event',
        ))
        fixture = (ROOT / 'tests/host/synth_pointer_test.inc').read_text()
        code = fixture.replace('/* TYPES */', declarations).replace('/* FUNCTIONS */', functions)
        with tempfile.TemporaryDirectory() as directory:
            path, binary = Path(directory, 'synth.c'), Path(directory, 'synth')
            path.write_text(code)
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g', '-O1',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I' + str(ROOT / 'tests/host'), '-I' + str(ROOT / 'src/services'),
                '-I', str(ROOT / 'include'),
                str(path), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
