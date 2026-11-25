"""Common TUI/GUI selection model works from an isolated public SDK."""
from pathlib import Path
import shutil
import json
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class UiViewportTest(unittest.TestCase):
    def test_isolated_sdk_navigation_and_size_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            for name in ('solar_os_native_client_ui.h', 'solar_os_native_client_ui_utf8.h', 'solar_os_tui_types_v1.h', 'solar_os_key_codes.h'):
                shutil.copyfile(ROOT / 'include' / name, stage / name)
            binary = stage / 'viewport'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-pie', '-no-pie',
                '-I', str(stage), str(ROOT / 'tests/host/ui_viewport_test.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_public_helper_compiles_as_cpp(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'viewport.cpp'
            source.write_text('#include "solar_os_native_client_ui.h"\n'
                'int main(){solar_os_ui_viewport_t view={0,0};'
                'solar_os_ui_viewport_key(&view,SOLAR_OS_KEY_UP,12,5,true);return view.cursor==11?0:1;}\n')
            binary = Path(directory) / 'viewport'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                '-I', str(ROOT / 'include'), str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    @unittest.skipUnless((ROOT / '.pio/tinycc/solar-tcc').is_file(), 'OS C frontend required')
    def test_os_c_frontend_emits_a_loader_valid_module(self):
        from test_external_library_sdk import builtin_headers
        cache = ROOT / '.pio/tinycc'
        pin = json.loads((ROOT / 'modules/experimental/tinycc/upstream.json').read_text())['revision']
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            sdk = stage / 'sdk'; sdk.mkdir()
            for name, path in builtin_headers(cache / 'source' / ('tinycc-' + pin)).items():
                shutil.copyfile(path, sdk / name)
            source, obj, artifact = stage / 'viewport.c', stage / 'viewport.o', stage / 'viewport.elf'
            source.write_text('#include "solar_os_native_abi.h"\n#include "solar_os_native_client_ui.h"\n'
                'int main(int argc,char **argv){(void)argv;solar_os_ui_viewport_t view={0,0};'
                'solar_os_ui_viewport_key(&view,SOLAR_OS_KEY_UP,argc>0?(size_t)argc:12,5,true);'
                'char text[16]="ab";solar_os_ui_input_state_t state={1,0};'
                'solar_os_ui_input_text(text,sizeof(text),&state,"\\xc3\\xb6",2,8);'
                'return solar_os_native_host_v1()->write_utf8(view.cursor?"selected":"empty",5);}\n')
            subprocess.run([str(cache / 'solar-tcc'), str(source), str(obj), str(sdk)], check=True)
            subprocess.run([str(cache / 'module'), str(obj), str(artifact), 'main'], check=True)
            subprocess.run([str(cache / 'elf-check'), str(artifact)], check=True)
