"""The generic SDK builder produces both forms from one closed object graph."""
from pathlib import Path
import dataclasses
import shutil
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from build_native_component import build, load_spec, inputs, symbols
from generate_flavor_config import load_catalog, load_flavor, generate_cmake
from solaros_components import load_builtin_components

CC = Path.home() / '.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc'
PREFIX = str(CC)[:-3]


def fixture(root, *, cpp=False, foreground=False):
    root.mkdir(parents=True, exist_ok=True)
    manifest = root / 'module.toml'
    manifest.write_text('''schema_version=2
type="app"
id="build-probe"
name="Build probe"
version="1.0.0"
native_abi=1
target="esp32s3"
lifecycle_abi=''' + ('2' if foreground else '1') + '''
artifact="build-probe.app.elf"
[builtin]
package="app_cc"
entry="solar_os_build_probe_entry"
''' + ('manifest="solar_os_build_probe_manifest"\n' if foreground else '') + '''
[build]
entry="probe_main"
sources=["probe.''' + ('cpp' if cpp else 'c') + '''"]
include_dirs=["."]
defines={PROBE_VALUE=7}
''')
    text = '''#include "solar_os_native_abi.h"
int shared_name(void) { return PROBE_VALUE; }
''' + ('extern "C" ' if cpp else '') + '''int probe_main(int argc, char **argv) {
    (void)argc; (void)argv;
    const solar_os_native_host_api_v1_t *host=solar_os_native_host_v1();
    return host && host->write_utf8 ? host->write_utf8("probe", shared_name()) : 1;
}
'''
    if foreground:
        text += '''#include "solar_os_native_app_abi.h"
SOLAR_OS_NATIVE_APP_MANIFEST = {
    .magic=SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC,
    .lifecycle_abi=2, .struct_size=sizeof(solar_os_native_app_manifest_v2_t),
    .app_class=SOLAR_OS_NATIVE_APP_CLASS_TUI, .id="build-probe", .summary="Probe",
    .worker_stack_bytes=4096, .worker_priority=1, .tick_interval_ms=20
};
'''
    (root / ('probe.cpp' if cpp else 'probe.c')).write_text(text)
    return manifest


class NativeComponentSpecTest(unittest.TestCase):
    def test_flavor_selects_only_enabled_generic_builds(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = fixture(root / 'modules/probe')
            component, = load_builtin_components(root, {'app_cc'})
            self.assertEqual(component.build, manifest.resolve())
            catalog = load_catalog(ROOT / 'packages/solar_os_packages.toml')
            catalog = dataclasses.replace(catalog, components=(component,))
            flavor = ROOT / 'flavors/full.toml'
            _, _, groups, enabled = load_flavor(flavor, catalog)
            for flag in (True, False):
                enabled['app_cc'] = flag
                cmake = generate_cmake('probe', '', groups, enabled, catalog, flavor,
                                       ROOT / 'packages/solar_os_packages.toml')
                self.assertEqual(str(manifest) in cmake, flag)

    def test_inputs_reject_escapes_duplicates_and_bad_export_names(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = fixture(root / 'module')
            source = manifest.read_text()
            (root / 'outside.c').write_text('int outside;\n')
            for old, new, message in (
                ('sources=["probe.c"]', 'sources=["../outside.c"]', 'escapes'),
                ('sources=["probe.c"]', 'sources=["probe.c","probe.c"]', 'duplicate'),
                ('entry="solar_os_build_probe_entry"', 'entry="bad();"', 'export'),
                ('defines={PROBE_VALUE=7}', 'defines={"bad name"=1}', 'defines'),
                ('sources=["probe.c"]', 'sources=[]', 'requires'),
            ):
                with self.subTest(message=message):
                    manifest.write_text(source.replace(old, new))
                    with self.assertRaisesRegex(ValueError, message):
                        load_spec(manifest)
            manifest.write_text(source)
            (manifest.parent / 'local.h').write_text('#define LOCAL 1\n')
            self.assertIn(manifest.parent / 'local.h', inputs(load_spec(manifest)))


@unittest.skipUnless(CC.is_file(), 'Xtensa compiler is installed by firmware job')
class NativeComponentBuildTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        cls.validator = cls.root / 'elf-check'
        subprocess.run(['cc', '-std=c11', '-O2', '-include', 'stdbool.h',
            '-I' + str(ROOT / 'tests/host'), '-I' + str(ROOT / 'include'),
            '-I' + str(ROOT / 'src/services'), str(ROOT / 'modules/experimental/tinycc/elf_check.c'),
            str(ROOT / 'src/services/solar_os_native_elf.c'), '-o', str(cls.validator)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_archive_and_elf_share_sources_exports_and_closed_imports(self):
        for cpp, foreground in ((False, False), (True, False), (False, True)):
            with self.subTest(cpp=cpp, foreground=foreground), tempfile.TemporaryDirectory(dir=self.root) as directory:
                root = Path(directory)
                manifest = fixture(root / 'source', cpp=cpp, foreground=foreground)
                spec = load_spec(manifest)
                report = build(spec, root / 'out', CC)
                self.assertEqual(set(report['artifacts']), {'builtin', 'external'})
                self.assertEqual(set(report['imports']), {'solar_os_native_host_v1'})
                archive = root / 'out' / report['artifacts']['builtin']
                # nm includes an object-name heading for archives, so inspect the member.
                subprocess.run([PREFIX + 'ar', 'x', str(archive)], cwd=root, check=True)
                self.assertEqual(symbols(PREFIX + 'nm', root / 'component.o', True),
                                 {replacement for _, replacement in spec.exports})
                self.assertEqual(symbols(PREFIX + 'nm', root / 'component.o'), {'solar_os_native_host_v1'})
                artifact = root / 'out' / report['artifacts']['external']
                subprocess.run([str(self.validator), str(artifact)], check=True)
                if foreground:
                    # The loader discovers this exact symbol; it is not an arbitrary export.
                    dynamic = subprocess.check_output([PREFIX + 'nm', '-D', '--defined-only', str(artifact)], text=True)
                    self.assertIn('solar_os_native_app_manifest', dynamic)
                    self.assertNotIn('solar_os_build_probe_manifest', dynamic)

    def test_archive_input_and_private_import_rejection_before_publication(self):
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            root = Path(directory)
            manifest = fixture(root / 'source')
            spec = load_spec(manifest)
            # Precompiled third-party components can close their object graph here.
            subprocess.run([str(CC), '-fPIC', '-DPROBE_VALUE=7', '-I' + str(ROOT / 'include'),
                '-c', str(spec.sources[0]), '-o', str(root / 'source/prebuilt.o')], check=True)
            subprocess.run([PREFIX + 'ar', 'rcs', str(root / 'source/prebuilt.a'),
                            str(root / 'source/prebuilt.o')], check=True)
            manifest.write_text(manifest.read_text().replace('sources=["probe.c"]', 'archives=["prebuilt.a"]'))
            build(load_spec(manifest), root / 'archive', CC)
            original = (root / 'archive/build-probe.app.elf').read_bytes()
            manifest.write_text(manifest.read_text().replace('archives=["prebuilt.a"]', 'sources=["probe.c"]'))
            spec.sources[0].write_text(spec.sources[0].read_text() +
                '\nextern int private_backend(void);\nint force_private(void) { return private_backend(); }\n')
            with self.assertRaisesRegex(ValueError, 'unclosed component imports: private_backend'):
                build(load_spec(manifest), root / 'archive', CC)
            self.assertEqual((root / 'archive/build-probe.app.elf').read_bytes(), original)

    def test_cmake_builds_links_and_rebuilds_selected_archive(self):
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            root = Path(directory)
            manifest = fixture(root / 'source')
            (root / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.16)
project(component_probe C CXX)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(IDF_TARGET esp32s3)
set(SOLAR_OS_PROJECT_DIR "''' + str(ROOT) + '''")
set(SOLAR_OS_COMPONENT_BUILD_MANIFESTS "''' + str(manifest) + '''")
add_executable(consumer consumer.c)
target_include_directories(consumer PRIVATE "''' + str(ROOT / 'include') + '''")
target_link_options(consumer PRIVATE -nostdlib -Wl,-e,main -Wl,--no-undefined)
include("''' + str(ROOT / 'cmake/solar_os_components.cmake') + '''")
solar_os_link_components(consumer)
''')
            (root / 'consumer.c').write_text('''#include "solar_os_native_abi.h"
extern int solar_os_build_probe_entry(int, char **);
static int write_text(const char *p, size_t n) { (void)p; return (int)n; }
static const solar_os_native_host_api_v1_t host={.write_utf8=write_text};
const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void) { return &host; }
int main(void) { return solar_os_build_probe_entry(0, 0); }
''')
            subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'build'),
                '-DCMAKE_C_COMPILER=' + str(CC), '-DCMAKE_CXX_COMPILER=' + PREFIX + 'g++',
                '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY'], check=True, stdout=subprocess.DEVNULL)
            subprocess.run(['cmake', '--build', str(root / 'build')], check=True, stdout=subprocess.DEVNULL)
            archive = root / 'build/native-components/build-probe/libsolar_os_build-probe.a'
            self.assertTrue(archive.is_file())
            self.assertEqual(symbols(PREFIX + 'nm', root / 'build/consumer'), set())
            before = archive.read_bytes()
            source = manifest.parent / 'probe.c'
            source.write_text(source.read_text().replace('return PROBE_VALUE;', 'return PROBE_VALUE + 1;'))
            subprocess.run(['cmake', '--build', str(root / 'build')], check=True, stdout=subprocess.DEVNULL)
            self.assertNotEqual(archive.read_bytes(), before)

    def test_library_build_embeds_its_public_sdk(self):
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            root = Path(directory)
            example = ROOT / 'modules/examples/hello-library'
            shutil.copytree(example / 'main', root / 'main')
            manifest = root / 'module.toml'
            manifest.write_text((example / 'module.toml').read_text())
            report = build(load_spec(manifest), root / 'out', CC)
            artifact = root / 'out' / report['artifacts']['external']
            subprocess.run([str(self.validator), str(artifact)], check=True)
            self.assertIn(b'hello-library/hello_library_api.h', artifact.read_bytes())

    def test_bad_foreground_metadata_never_publishes_either_form(self):
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            root = Path(directory)
            manifest = fixture(root / 'source', foreground=True)
            source = manifest.parent / 'probe.c'
            source.write_text(source.read_text().replace('worker_stack_bytes=4096', 'worker_stack_bytes=1'))
            with self.assertRaises(subprocess.CalledProcessError):
                build(load_spec(manifest), root / 'out', CC)
            self.assertFalse(list((root / 'out').glob('*.a')))
            self.assertFalse(list((root / 'out').glob('*.elf')))

    def test_platformio_schedules_only_selected_components_and_tracks_inputs(self):
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            root = Path(directory)
            fixture(root / 'modules/probe')
            (root / 'scripts').symlink_to(ROOT / 'scripts', target_is_directory=True)
            config = root / 'build/generated/solar_os/solar_os_config.h'
            config.parent.mkdir(parents=True)
            script = ROOT / 'scripts/platformio_components.py'

            class Env:
                def __init__(self): self.commands = []; self.libraries = []
                def subst(self, name):
                    return {'$PROJECT_DIR': str(root), '$BUILD_DIR': str(root / 'build'),
                            '$CC': str(CC), '$CXX': PREFIX + 'g++'}[name]
                def BoardConfig(self): return {'build.mcu': 'esp32s3'}
                def WhereIs(self, name): return name
                def VerboseAction(self, command, label): return command
                def Command(self, archive, dependencies, action):
                    self.commands.append((archive, dependencies, action)); return archive
                def Depends(self, target, node): pass
                def File(self, name): return name
                def Append(self, **kwargs): self.libraries.extend(kwargs['LIBS'])

            for enabled in (True, False):
                config.write_text('#define SOLAR_OS_PACKAGE_APP_CC ' + str(int(enabled)) + '\n')
                env = Env()
                exec(compile(script.read_text(), str(script), 'exec'), {'env': env, 'Import': lambda name: None})
                self.assertEqual(len(env.commands), int(enabled))
                if enabled:
                    archive, dependencies, action = env.commands[0]
                    self.assertIn(str(root / 'modules/probe/probe.c'), dependencies)
                    self.assertEqual(env.libraries, [archive])
                    subprocess.run(shlex.split(action), check=True, stdout=subprocess.DEVNULL)
                    self.assertTrue(Path(archive).is_file())

    def test_unmanaged_global_constructors_are_rejected(self):
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            root = Path(directory)
            manifest = fixture(root / 'source', cpp=True)
            source = manifest.parent / 'probe.cpp'
            source.write_text(source.read_text() + '''
volatile int constructor_seen;
struct Object { Object() { constructor_seen=1; } } object;
''')
            with self.assertRaisesRegex(ValueError, 'explicit constructor/destructor lifecycle'):
                build(load_spec(manifest), root / 'out', CC)

    def test_private_relative_includes_cannot_bypass_the_sdk_boundary(self):
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            root = Path(directory)
            manifest = fixture(root / 'source')
            (root / 'private.h').write_text('#define FIRMWARE_PRIVATE 1\n')
            source = manifest.parent / 'probe.c'
            source.write_text('#include "../private.h"\n' + source.read_text())
            with self.assertRaisesRegex(ValueError, 'non-SDK header outside its module'):
                build(load_spec(manifest), root / 'out', CC)


if __name__ == '__main__':
    unittest.main()
