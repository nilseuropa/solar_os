from pathlib import Path
import json
import os
import re
import shlex
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class NativeCompiledForegroundTest(unittest.TestCase):
    def test_complete_host_and_registry_compile_for_classic_without_loader_imports(self):
        environment = os.environ.get('SOLAR_OS_NATIVE_CORE_BUILD', 'freenove_esp32_wrover_v3')
        database = ROOT / '.pio/build' / environment / 'compile_commands.json'
        if not database.exists():
            self.skipTest('build a classic target to check the complete foreground host')
        records = json.loads(database.read_text())
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            override = stage / 'compiled_foreground_config.h'
            override.write_text('#include "solar_os_config.h"\n'
                '_Static_assert(!SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES, "requires a firmware without ELF loading");\n'
                '#undef SOLAR_OS_NATIVE_FOREGROUND_ENABLED\n#define SOLAR_OS_NATIVE_FOREGROUND_ENABLED 1\n')
            for filename in ('solar_os_native_foreground.c', 'solar_os_app_registry.c'):
                with self.subTest(source=filename):
                    record = next(row for row in records if Path(row['file']).name == filename)
                    args = shlex.split(record['command'])
                    binary = stage / (filename + '.o')
                    args[args.index('-o') + 1] = str(binary)
                    args += ['-include', str(override)]
                    subprocess.run(args, cwd=record['directory'], check=True, capture_output=True, text=True)
                    nm = str(Path(args[0]).with_name(Path(args[0]).name.removesuffix('gcc') + 'nm'))
                    undefined = subprocess.check_output([nm, '-u', str(binary)], text=True)
                    names = {line.split()[-1] for line in undefined.splitlines() if line.split()}
                    self.assertNotIn('solar_os_native_run', names)
                    self.assertFalse(any('esp_elf' in name or name.startswith('solar_os_module_package_') for name in names))

    def test_shared_host_dispatch_identity_and_manifest_admission_without_elf(self):
        source = (ROOT / 'src/services/solar_os_native_foreground.c').read_text()
        slot = re.search(r'struct foreground_slot \{.*?\};', source, re.S).group()
        functions = production_functions(source, ('foreground_find_slot', 'foreground_prepare_slot',
            'solar_os_native_foreground_resolve', 'solar_os_native_foreground_find', 'foreground_worker'))
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / 'compiled_foreground.inc').write_text(slot + '\n' + functions)
            binary = stage / 'foreground'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-DSOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES=0', '-g', '-O1',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-I', str(stage), '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'),
                '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/apps'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_compiled_foreground_test.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
            self.assertNotIn('esp_elf', subprocess.check_output(['nm', '-u', str(binary)], text=True))
