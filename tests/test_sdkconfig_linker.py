"""Exercise the production linker's configuration dependency with real SCons."""
import ast
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCONS = Path.home() / '.platformio/packages/tool-scons/scons.py'


class SdkconfigLinkerTest(unittest.TestCase):
    @unittest.skipUnless(SCONS.is_file() or shutil.which('scons'), 'SCons is required')
    def test_configuration_change_regenerates_linker_and_image(self):
        tree = ast.parse((ROOT / 'scripts/platformio_solaros_flavor.py').read_text())
        dependencies = '\n'.join(ast.unparse(node) for node in tree.body
            if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call)
            and isinstance(node.value.func, ast.Attribute)
            and ast.unparse(node.value.func) == 'env.Depends')
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            config = stage / 'build/config/sdkconfig.h'
            config.parent.mkdir(parents=True)
            config.write_text('#define CACHE_SIZE 0x4000\n')
            (stage / 'memory.ld.in').write_text('IRAM_START = 0x40370000 + CACHE_SIZE;\n')
            (stage / 'SConstruct').write_text('''
from pathlib import Path
env = Environment(BUILD_DIR='build')
''' + dependencies + '''
def preprocess(target, source, env):
    size = Path('build/config/sdkconfig.h').read_text().split()[-1]
    Path(str(target[0])).write_text(Path(str(source[0])).read_text().replace('CACHE_SIZE', size))
def link(target, source, env):
    Path(str(target[0])).write_bytes(Path(str(source[0])).read_bytes())
intermediate = env.Command('$BUILD_DIR/esp-idf/esp_system/ld/memory.ld.in', 'memory.ld.in', preprocess)
env.Command('$BUILD_DIR/memory.ld', intermediate, link)
image = env.Command('$BUILD_DIR/firmware.bin', '$BUILD_DIR/memory.ld', link)
Default(image)
''')
            command = [sys.executable, str(SCONS)] if SCONS.is_file() else [shutil.which('scons')]
            def build():
                subprocess.run(command + ['-Q'], cwd=stage, check=True, capture_output=True, text=True)
            build()
            image = stage / 'build/firmware.bin'
            self.assertIn('0x4000', image.read_text())
            unchanged = image.stat().st_mtime_ns
            build()
            self.assertEqual(image.stat().st_mtime_ns, unchanged)
            config.write_text('#define CACHE_SIZE 0x8000\n')
            build()
            self.assertIn('0x8000', image.read_text())
            config.write_text('#define CACHE_SIZE 0x4000\n')
            build()
            self.assertIn('0x4000', image.read_text())
