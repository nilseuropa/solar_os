import copy
import importlib.util
import json
import re
from pathlib import Path
import shutil
import subprocess
import sys
import types
import tempfile
import tomllib
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('generate_native_api', ROOT/'scripts/generate_native_api.py')
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)
sys.path.insert(0, str(ROOT/'scripts'))
import audit_native_system as audit


class NativeApiContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.headers, cls.services = api.load(ROOT)
        cls.baseline = api.snapshot(cls.headers, cls.services)

    def test_generated_sdk_and_released_baseline_are_current(self):
        api.main(['--check'])

    def test_new_root_provider_compiles_with_generated_declarations(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.clone(temporary)
            metadata = root / 'api/native/services.toml'
            metadata.write_text(metadata.read_text() + '\n[[provider]]\nparent="root"\n'
                'getter="solar_os_native_fixture_extra_get_service"\n')
            headers, services = api.load(root)
            outputs = api.outputs(root, headers, services)
            generated = root / 'src/services/generated'
            generated.mkdir(parents=True, exist_ok=True)
            for name in ('native_root_provider_headers.inc', 'native_root_providers.inc'):
                path = Path('src/services/generated') / name
                (root / path).write_text(outputs[path])
            providers = tomllib.loads(metadata.read_text())['provider']
            for provider in providers:
                if provider['parent'] != 'root':
                    continue
                header = provider['getter'].removesuffix('_get_service') + '.h'
                target = root / 'src/services' / header
                if header == 'solar_os_native_fixture_extra.h':
                    target.write_text('#include <stdint.h>\nconst void *solar_os_native_fixture_extra_get_service('
                                      'const char *,uint32_t,uint32_t);\n')
                else:
                    shutil.copyfile(ROOT / 'src/services' / header, target)
            source = root / 'providers.c'
            source.write_text('#include <stddef.h>\n#include <stdint.h>\n'
                '#include "generated/native_root_provider_headers.inc"\n'
                'static const void *lookup(const char *name,uint32_t abi,uint32_t minimum){\n'
                '#include "generated/native_root_providers.inc"\nreturn NULL;}\n'
                'int main(void){return lookup("fixture",1,8)==NULL;}\n')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-c', str(source),
                '-I', str(root / 'src/services'), '-I', str(ROOT / 'include'),
                '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                '-I', str(ROOT / 'tests/host'), '-o', str(root / 'providers.o')], check=True)

    def test_removing_root_providers_clears_dispatch_and_declarations(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.clone(temporary)
            metadata = root / 'api/native/services.toml'
            text = re.sub(r'\[\[provider\]\]\n.*?(?=\n\[\[|\Z)', '', metadata.read_text(), flags=re.S)
            metadata.write_text(text)
            headers, services = api.load(root)
            outputs = api.outputs(root, headers, services)
            for name in ('native_root_provider_headers.inc', 'native_root_providers.inc'):
                content = outputs[Path('src/services/generated') / name]
                self.assertNotIn('_get_service', content)

    def test_system_coverage_is_current_and_complete(self):
        audit.main(['--check', '--require-full'])

    def system_fixture(self, tmp):
        root=self.clone(tmp)
        (root/'packages').mkdir(exist_ok=True)
        (root/'packages/solar_os_packages.toml').write_text(
            '[packages.service_example]\nsources=["services/example.c"]\n')
        (root/'src/services').mkdir(parents=True,exist_ok=True)
        (root/'src/services/example.c').write_text('')
        (root/'src/services/example.h').write_text('int solar_os_example_read(void);\n')
        (root/'api/native/system.toml').write_text(
            '[scope]\npackages=["service_*"]\nexclude_packages=[]\nexclude_paths=[]\n[operation]\n')
        return root

    def test_new_os_declarations_and_misspelled_operations_are_detected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=self.system_fixture(tmp)
            audit.main(['--root',str(root),'--update-lock'])
            path=root/'src/services/example.h'
            path.write_text(path.read_text()+'int solar_os_example_write(int value);\n')
            with self.assertRaisesRegex(ValueError,'coverage changed'):
                audit.main(['--root',str(root),'--check'])
            meta=root/'api/native/system.toml'
            meta.write_text(meta.read_text()+'solar_os_example_read=["memory.nonexistent"]\n')
            with self.assertRaisesRegex(ValueError, 'unknown ABI operation'):
                audit.inventory(root)

    def test_os_inventory_is_independent_of_application_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=self.system_fixture(tmp)
            source=root/'src/apps/application.c'
            source.parent.mkdir(parents=True)
            source.write_text('void app(void) { solar_os_other_operation(); }')
            self.assertEqual(set(audit.inventory(root)),{'solar_os_example_read'})

    def test_inventory_detects_multiline_return_types_and_attributes(self):
        declarations = audit.declarations('''
esp_err_t
solar_os_multiline(void);
const solar_os_thing_t *
solar_os_pointer(int value);
esp_err_t solar_os_log_example(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
typedef void (*solar_os_callback_t)(void);
static int solar_os_private(void);
''')
        self.assertEqual(set(declarations), {'solar_os_multiline', 'solar_os_pointer', 'solar_os_log_example'})

    def test_explicit_internal_review_is_separate_from_public_operation_coverage(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=self.system_fixture(tmp)
            entry=audit.inventory(root)['solar_os_example_read']
            meta=root/'api/native/system.toml'
            meta.write_text(meta.read_text()+'\n[internal.solar_os_example_read]\n'
                'reason="Called only by core boot to initialize the event pump."\n'
                f'review="{entry["review_revision"]}"\n')
            entry=audit.inventory(root)['solar_os_example_read']
            self.assertFalse(entry['operations'])
            self.assertTrue(entry['internal_reason'])
            report = root / 'inventory.json'
            audit.main(['--root',str(root),'--require-full','--report',str(report)])
            self.assertEqual(json.loads(report.read_text())['solar_os_example_read'], entry)

    def test_internal_review_cannot_hide_unknown_mapped_or_unreviewed_declarations(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=self.system_fixture(tmp)
            meta=root/'api/native/system.toml';original=meta.read_text()
            revision=audit.inventory(root)['solar_os_example_read']['review_revision']
            review=f'\n[internal.solar_os_example_read]\nreason="Core boot hook."\nreview="{revision}"\n'
            cases=[(original+review.replace('solar_os_example_read','solar_os_typo'),'unknown OS declaration'),
                (original+review.replace('Core boot hook.',' '),'needs a reason'),
                (original+review.replace(revision,'stale'),'internal review changed'),
                (original+'solar_os_example_read=["memory.alloc"]\n'+review,'both mapped and internal')]
            for text,error in cases:
                with self.subTest(error=error):
                    meta.write_text(text)
                    with self.assertRaisesRegex(ValueError,error):audit.inventory(root)
            meta.write_text(original+review)
            header=root/'src/services/example.h'
            header.write_text('int solar_os_example_read(int flags);\n')
            with self.assertRaisesRegex(ValueError,'internal review changed'):audit.inventory(root)

    def test_internal_review_pins_local_types_and_leaves_new_declarations_pending(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=self.system_fixture(tmp)
            header=root/'src/services/example.h';meta=root/'api/native/system.toml'
            types=root/'include/solar_os_example_types.h'
            types.write_text('typedef struct { int value; } solar_os_example_t;\n')
            header.write_text('#include "solar_os_example_types.h"\nint solar_os_example_read(solar_os_example_t *value);\n')
            original=meta.read_text()
            def review():
                revision=audit.inventory(root)['solar_os_example_read']['review_revision']
                meta.write_text(original+'\n[internal.solar_os_example_read]\nreason="Core boot hook."\n'
                    f'review="{revision}"\n')
            review()
            types.write_text('typedef struct { int value, flags; } solar_os_example_t;\n')
            with self.assertRaisesRegex(ValueError,'internal review changed'):audit.inventory(root)
            header.write_text(header.read_text()+'int solar_os_example_write(void);\n')
            meta.write_text(original);review()
            entries=audit.inventory(root)
            self.assertTrue(entries['solar_os_example_read']['internal_reason'])
            self.assertFalse(entries['solar_os_example_write']['operations'])
            self.assertFalse(entries['solar_os_example_write']['internal_reason'])
            with self.assertRaisesRegex(ValueError,'coverage is incomplete'):
                audit.main(['--root',str(root),'--require-full'])

    def test_existing_os_record_and_constant_changes_require_coverage_review(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=self.system_fixture(tmp)
            header=root/'src/services/example.h'
            types=root/'include/solar_os_example_types.h'
            types.write_text('#define SOLAR_OS_EXAMPLE_FLAG 1U\ntypedef struct { int value; } solar_os_example_t;\n')
            header.write_text('#include "solar_os_example_types.h"\nint solar_os_example_read(void);\n')
            audit.main(['--root',str(root),'--update-lock'])
            for changed in ('#define SOLAR_OS_EXAMPLE_FLAG 2U\ntypedef struct { int value; } solar_os_example_t;\n',
                            '#define SOLAR_OS_EXAMPLE_FLAG 1U\ntypedef struct { int value, extra; } solar_os_example_t;\n'):
                types.write_text(changed)
                with self.assertRaisesRegex(ValueError,'coverage changed'):
                    audit.main(['--root',str(root),'--check'])

    def test_compatible_table_extension_keeps_old_prefix_and_allows_new_abi(self):
        current = copy.deepcopy(self.baseline)
        current['types']['solar_os_native_time_api_v1_t']['table'].append('int ( * optional ) ( void )')
        current['types']['solar_os_native_time_api_v2_t'] = {'table': ['uint32_t abi_version , struct_size']}
        current['services']['time_v2'] = {'name':'time', 'abi':2, 'type':'solar_os_native_time_api_v2_t', 'scoped':False}
        self.assertEqual(api.compatibility_errors(self.baseline, current), [])

    def test_changed_removed_and_reordered_methods_are_rejected(self):
        for action in ('remove', 'reorder', 'signature'):
            with self.subTest(action=action):
                current = copy.deepcopy(self.baseline)
                fields = current['types']['solar_os_native_thread_api_v1_t']['table']
                if action == 'remove': fields.pop()
                elif action == 'reorder': fields[1], fields[2] = fields[2], fields[1]
                else: fields[1] = fields[1].replace('uint32_t', 'uint64_t')
                self.assertTrue(api.compatibility_errors(self.baseline, current))

    def test_records_enums_constants_and_shared_include_layouts_are_frozen(self):
        mutations = [
            ('constants', 'SOLAR_OS_NATIVE_STORAGE_PATH_MAX', '320U'),
            ('types', 'solar_os_native_storage_entry_v1_t', {'declaration':'changed'}),
            ('types', 'solar_os_raster_image_pixels_t', {'declaration':'changed'}),
            ('types', 'solar_os_native_library_descriptor_v1_t', {'declaration':'changed'}),
            ('services', 'time', {'name':'renamed', 'abi':1, 'type':'changed', 'scoped':False}),
        ]
        # Enum members are part of their containing declaration, not macros.
        for section, name, value in mutations:
            self.assertIn(name, self.baseline[section])
            with self.subTest(name=name):
                current = copy.deepcopy(self.baseline)
                current[section][name] = value
                self.assertTrue(api.compatibility_errors(self.baseline, current))
        current = copy.deepcopy(self.baseline)
        name = 'solar_os_native_service_result_v1_t'
        current['types'][name]['declaration'] += ' changed'
        self.assertTrue(api.compatibility_errors(self.baseline, current))

    def test_comments_and_whitespace_do_not_change_the_contract(self):
        headers = {name:'/* harmless */\n' + text.replace(';', ';\n\n') for name,text in self.headers.items()}
        self.assertEqual(self.baseline, api.snapshot(headers, self.services))

    def test_parameter_names_and_nested_callback_names_do_not_change_the_abi(self):
        headers = dict(self.headers)
        name = 'solar_os_native_thread_abi.h'
        headers[name] = headers[name].replace('void (*destructor)(void *)', 'void (*cleanup)(void *)')
        headers[name] = headers[name].replace('uint32_t stack_bytes', 'uint32_t capacity')
        headers[name] = headers[name].replace('void *argument', 'void *user')
        self.assertEqual(self.baseline, api.snapshot(headers, self.services))

    def test_host_exports_and_multiline_manifest_macros_are_protected(self):
        for header, before, after in (
            ('solar_os_native_abi.h','solar_os_native_host_v1(void)','solar_os_native_wrong_v1(void)'),
            ('solar_os_native_library_abi.h','solar_os_native_library_host_v1(void)','solar_os_native_library_host_v1(int)'),
            ('solar_os_native_app_abi.h','solar_os_native_app_manifest','solar_os_wrong_manifest'),
        ):
            with self.subTest(header=header):
                headers = dict(self.headers)
                headers[header] = headers[header].replace(before, after)
                self.assertTrue(api.compatibility_errors(self.baseline, api.snapshot(headers,self.services)))

    def clone(self, directory):
        root = Path(directory)
        shutil.copytree(ROOT/'api', root/'api')
        shutil.copytree(ROOT/'include', root/'include')
        shutil.copytree(ROOT/'packages', root/'packages')
        for relative, text in api.outputs(ROOT, self.headers, self.services).items():
            path = root/relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        return root

    def test_unknown_package_gate_cannot_silently_disable_a_service(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.clone(tmp)
            path = root/'api/native/services.toml'
            source = path.read_text()
            changed = re.sub(r'(?m)^gate = "SOLAR_OS_PACKAGE_[^"]+"$',
                'gate = "SOLAR_OS_PACKAGE_SERVICE_DOES_NOT_EXIST"', source, count=1)
            self.assertNotEqual(source, changed)
            path.write_text(changed)
            with self.assertRaisesRegex(ValueError, 'unknown native service package gate'):
                api.main(['--root', str(root), '--check'])

    def test_public_records_cannot_bypass_typedef_layout_tracking(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.clone(tmp)
            path = root/'api/native/definitions/solar_os_native_time_abi.h'
            path.write_text(path.read_text()+'\nstruct untracked { int value; };\n')
            with self.assertRaisesRegex(ValueError, 'must use a typedef'):
                api.load(root)

    def test_rehoming_last_root_table_clears_and_checks_the_dispatch_anchor(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=self.clone(tmp)
            metadata=root/'api/native/services.toml'
            metadata.write_text(metadata.read_text().replace('provider = "root_late"','provider = "separate"'))
            anchor=root/'src/services/generated/native_root_late_dispatch.inc'
            anchor.write_text('#error stale table left in root\n')
            with self.assertRaisesRegex(ValueError,'regenerate native API'):
                api.main(['--root',str(root),'--check'])
            api.main(['--root',str(root)])
            self.assertNotIn('stale table',anchor.read_text())
            self.assertNotIn('return &',anchor.read_text())
            api.main(['--root',str(root),'--check'])

    def test_stale_generated_output_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.clone(tmp)
            (root/'include/solar_os_native_client.h').write_text('stale')
            with self.assertRaisesRegex(ValueError, 'regenerate native API'):
                api.main(['--root', str(root), '--check'])

    def test_platformio_also_refuses_a_stale_sdk_and_allows_clean(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.clone(tmp)
            (root/'include/solar_os_native_client.h').write_text('stale')
            script = (ROOT/'scripts/platformio_native_elf.py').read_text()
            module, scons = types.ModuleType('SCons.Script'), types.ModuleType('SCons')
            module.GetOption = lambda _: False
            class Environment:
                def subst(self, text): return str(root) if text == '$PROJECT_DIR' else str(root/'build')
            with patch.dict(sys.modules, {'SCons':scons, 'SCons.Script':module}):
                with self.assertRaisesRegex(SystemExit, 'SolarOS native API check failed: regenerate'):
                    exec(script, {'Import':lambda _:None, 'env':Environment()})
                module.GetOption = lambda _: True
                exec(script, {'Import':lambda _:None, 'env':Environment()})

    def test_update_lock_cannot_bless_a_breaking_change(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.clone(tmp)
            lock = (root/'api/native/abi-lock.json').read_bytes()
            path = root/'api/native/definitions/solar_os_native_time_abi.h'
            path.write_text(path.read_text().replace('int64_t (*now_us)', 'int32_t (*now_us)'))
            with self.assertRaisesRegex(ValueError, 'incompatible types'):
                api.main(['--root', str(root), '--update-lock'])
            self.assertEqual((root/'api/native/abi-lock.json').read_bytes(), lock)

    def test_vendor_headers_and_configuration_are_rejected(self):
        for extra in ('#include "esp_err.h"\n', '#define FLAG CONFIG_FREERTOS_HZ\n'):
            with self.subTest(extra=extra), tempfile.TemporaryDirectory() as tmp:
                root = self.clone(tmp)
                path = root/'api/native/definitions/solar_os_native_time_abi.h'
                path.write_text(path.read_text() + extra)
                with self.assertRaisesRegex(ValueError, 'private/vendor|firmware configuration'):
                    api.load(root)

    def test_public_sdk_compiles_without_firmware_headers_in_c_and_cpp(self):
        source = '#include "solar_os_native_client.h"\n'
        for compiler, language in (('cc','c'), ('c++','c++')):
            subprocess.run([compiler, '-x', language, '-Wall', '-Wextra', '-Werror', '-pedantic',
                            '-fsyntax-only', '-I', str(ROOT/'include'), '-'],
                           input=source, text=True, check=True)

    def test_client_rejects_wrong_headers_and_absent_optional_methods(self):
        source = r'''
#include <assert.h>
#include <stdlib.h>
#include "solar_os_native_client.h"
static solar_os_native_memory_api_v1_t table;
static void *allocate(size_t size) { return size ? &table : NULL; }
static const void *lookup(const char *name, uint32_t abi, uint32_t size)
{ (void)size; assert(!strcmp(name,"memory") && abi==1); return &table; }
int main(void) {
    assert(!solar_os_native_memory_get(NULL,8));
    assert(!solar_os_native_memory_get(lookup,0));
    assert(!solar_os_native_memory_get(lookup,8));
    table.abi_version=2;table.struct_size=sizeof(table);
    assert(!solar_os_native_memory_get(lookup,8));
    table.abi_version=1;table.struct_size=7;
    assert(!solar_os_native_memory_get(lookup,8));
    table.struct_size=SOLAR_OS_NATIVE_THROUGH(solar_os_native_memory_api_v1_t,alloc);
    assert(solar_os_native_memory_get(lookup,8)==&table);
    assert(!solar_os_native_memory_has_alloc(&table));
    table.alloc=allocate;assert(solar_os_native_memory_has_alloc(&table));
    assert(!solar_os_native_memory_has_free(&table));
    assert(!solar_os_native_memory_has_buffer_create(&table));
    /* An old provider really owns only the advertised prefix. ASan must see
     * no read of an appended pointer outside that allocation. */
    void *old=malloc(table.struct_size);assert(old);
    memcpy(old,&table,table.struct_size);
    assert(solar_os_native_memory_has_alloc(old));
    assert(!solar_os_native_memory_has_free(old));
    assert(!solar_os_native_memory_has_buffer_create(old));
    free(old);
    table.struct_size=sizeof(table);assert(!solar_os_native_memory_has_buffer_create(&table));
    assert(!solar_os_native_memory_has_alloc(NULL));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp)/'client'
            subprocess.run(['cc','-x','c','-std=c11','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-fno-pie','-no-pie',
                            '-I',str(ROOT/'include'),'-o',str(binary),'-'],
                           input=source,text=True,check=True)
            subprocess.run([str(binary)],check=True)

    def test_identical_module_runs_against_different_service_sets(self):
        with tempfile.TemporaryDirectory() as tmp:
            module = Path(tmp)/'probe.so'
            host = Path(tmp)/'host'
            subprocess.run(['cc','-shared','-nostdlib','-fPIC','-std=c11','-O2',
                            '-I',str(ROOT/'include'),
                            str(ROOT/'modules/examples/native-api-probe/main/native_api_probe.c'),
                            '-o',str(module)],check=True)
            imports = subprocess.check_output(['nm','-u',str(module)],text=True)
            symbols = {line.split()[-1] for line in imports.splitlines()}
            allowed = {'solar_os_native_host_v1','memcpy','memmove','memset','memcmp','strlen','setjmp','longjmp'}
            self.assertIn('solar_os_native_host_v1', symbols)
            self.assertFalse(symbols - allowed)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-rdynamic',
                            '-I',str(ROOT/'include'),str(ROOT/'tests/host/native_api_probe_host.c'),
                            '-ldl','-o',str(host)],check=True)
            original = module.read_bytes()
            for profile in ('display','headless'):
                subprocess.run([str(host),str(module),profile],check=True)
                self.assertEqual(module.read_bytes(), original)

    @unittest.skipUnless((ROOT/'.pio/tinycc/solar-tcc').is_file(), 'build the SolarOS C frontend first')
    def test_public_sdk_compiles_with_the_solaros_c_frontend(self):
        from test_external_library_sdk import builtin_headers
        cache = ROOT/'.pio/tinycc'
        pin = json.loads((ROOT/'modules/experimental/tinycc/upstream.json').read_text())['revision']
        with tempfile.TemporaryDirectory() as tmp:
            obj, elf = Path(tmp)/'probe.o', Path(tmp)/'probe.elf'
            sdk = Path(tmp) / 'sdk'; sdk.mkdir()
            for name, path in builtin_headers(cache/'source'/('tinycc-'+pin)).items():
                (sdk / name).write_bytes(path.read_bytes())
            subprocess.run([str(cache/'solar-tcc'),
                            str(ROOT/'modules/examples/native-api-probe/main/native_api_probe.c'),str(obj),
                            str(sdk)],check=True)
            subprocess.run([str(cache/'module'),str(obj),str(elf),'main'],check=True)
            subprocess.run([str(cache/'elf-check'),str(elf)],check=True)


if __name__ == '__main__': unittest.main()
