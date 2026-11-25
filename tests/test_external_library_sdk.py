import ast
import json
from pathlib import Path
import re
import subprocess
import tempfile
import tomllib
import unittest

from test_native_sdk import fixture, pack
from test_sdk_browser import build_browser

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'modules/experimental/tinycc'


def builtin_headers(source):
    # Exercise the production inventory without preparing or rebuilding TinyCC.
    tree = ast.parse((HERE / 'native_build.py').read_text())
    function = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                    and node.name == 'sdk_headers')
    namespace = {'ROOT': ROOT, 'HERE': HERE}
    exec(compile(ast.Module(body=[function], type_ignores=[]), 'native_build.py', 'exec'), namespace)
    return namespace['sdk_headers'](source)


class ExternalLibrarySdkTest(unittest.TestCase):
    def test_current_builtin_sdk_lists_shows_and_exports_within_file_bounds(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'include').mkdir()
            for name in ('stddef.h', 'stdbool.h', 'stdarg.h'):
                (root / 'include' / name).write_text('#pragma once\n')
            headers = builtin_headers(root)
            limit = int(re.search(r'#define SOLAR_OS_NATIVE_SDK_FILE_MAX (\d+)U',
                (ROOT / 'include/solar_os_native_sdk.h').read_text())[1])
            index = root / 'sdk_browser_builtin_headers.inc'
            entries = []
            for name, path in sorted(headers.items()):
                data = path.read_bytes()
                with self.subTest(header=name):
                    self.assertLessEqual(len(data), limit)
                entries.append('{%s, %s, %d},' % (json.dumps(name), json.dumps(data.decode()), len(data)))
            index.write_text('\n'.join(entries))
            browser = root / 'browser'; build_browser(browser, builtin_index=index)
            subprocess.run([str(browser), str(root), 'builtins'], check=True)
            for name, path in headers.items():
                with self.subTest(exported=name):
                    self.assertEqual((root / 'sdk' / name).read_bytes(), path.read_bytes())

    def test_builtin_sdk_contains_quoted_header_dependencies(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            (source / 'include').mkdir()
            for name in ('stddef.h', 'stdbool.h', 'stdarg.h'):
                (source / 'include' / name).write_text('#pragma once\n')
            headers = builtin_headers(source)
            self.assertIn('solar_os_cc_abi.h', headers)
            for name, path in headers.items():
                for dependency in re.findall(r'^#include "([^"]+)"', path.read_text(), re.M):
                    with self.subTest(header=name, dependency=dependency):
                        self.assertIn(dependency, headers)

    def test_all_external_library_sdks_list_show_export_and_compile(self):
        manifests = sorted((ROOT / 'modules').glob('*/module.toml'))
        manifests += sorted((ROOT / 'modules/examples').glob('*/module.toml'))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            browser = root / 'browser'
            build_browser(browser)
            builtins = builtin_headers(root)
            for manifest in manifests:
                meta = tomllib.loads(manifest.read_text())
                if meta['type'] != 'library':
                    continue
                if meta['id'] == 'tinycc':
                    # TinyCC publishes the built-in SDK rather than a second
                    # namespaced copy of its compiler API.
                    self.assertIn('solar_os_cc_abi.h', builtins)
                    continue
                with self.subTest(library=meta['id']):
                    public = ROOT / 'include' if manifest.parent.parent == ROOT / 'modules' else manifest.parent
                    module_id, version, headers = pack.load_headers(manifest, public)
                    stage = root / module_id
                    libraries = stage / 'modules/libraries'
                    libraries.mkdir(parents=True)
                    (libraries / (module_id + '.elf')).write_bytes(pack.pack_elf(fixture(), manifest, public))
                    subprocess.run([str(browser), str(stage), 'library', module_id, version,
                                    *[name for name, _, _ in headers]], check=True)
                    for name, data, _ in headers:
                        self.assertEqual((stage / 'sdk' / name).read_bytes(), data)
                    # Supply only the public SolarOS built-ins, leaving standard
                    # C headers to the cross compiler and excluding repo -I paths.
                    for name, path in builtins.items():
                        if name.startswith('solar_os_'):
                            (stage / 'sdk' / name).write_bytes(path.read_bytes())
                    code = stage / 'consumer.c'
                    code.write_text(''.join('#include <' + name + '>\n' for name, _, _ in headers)
                                    + 'int sdk_probe(void) { return 0; }\n')
                    compiler = Path.home() / '.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc'
                    subprocess.run([str(compiler) if compiler.is_file() else 'cc', '-std=c11',
                                    '-Wall', '-Wextra', '-Werror', '-I' + str(stage / 'sdk'),
                                    '-c', str(code), '-o', str(stage / 'consumer.o')], check=True)


if __name__ == '__main__':
    unittest.main()
