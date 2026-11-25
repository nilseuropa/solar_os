from pathlib import Path
import subprocess
import tempfile
import unittest
from test_keyboard_layouts import write_keyboard_layouts

ROOT = Path(__file__).resolve().parents[1]


class NativeInputCompositionTest(unittest.TestCase):
    def test_board_chords_independent_producer_masks_parallel_access_and_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            write_keyboard_layouts(directory)
            (stage / 'solar_os_board.h').write_text('#pragma once\n#include "solar_os_key_codes.h"\n#define SOLAR_OS_BOARD_HAS_SD 1\n'
                '#define SOLAR_OS_BOARD_INPUT_CHORDS { '
                '{SOLAR_OS_INPUT_MOD_LEFT_ALT,SOLAR_OS_KEY_UP,SOLAR_OS_KEY_PAGE_UP}, '
                '{SOLAR_OS_INPUT_MOD_LEFT_SHIFT,SOLAR_OS_KEY_LEFT,SOLAR_OS_KEY_HOME} }\n')
            binary = stage / 'composition'
            sources = ['solar_os_input.c', 'solar_os_keys.c', 'solar_os_input_composition.c', 'solar_os_native_input.c', 'solar_os_native_handles.c']
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1',
                '-Wall', '-Wextra', '-Werror', '-pthread', '-g', '-O1', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', str(stage), '-I', str(ROOT / 'tests/host/registry_stubs'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_input_composition_test.c'), str(ROOT / 'tests/host/native_no_library.c'),
                *[str(ROOT / 'src/services' / name) for name in sources], '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
