from pathlib import Path
import dataclasses
import json
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from solaros_components import load_builtin_components, selected_components
from generate_flavor_config import load_catalog, load_flavor, generate_header, apply_target_pruning


class ComponentsTest(unittest.TestCase):
    def manifest(self, root, identity="engine", package="backend_engine", dependencies=""):
        path = root / "modules" / identity / "module.toml"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('id=' + json.dumps(identity) + '\nname="Engine"\nversion="1.2.3"\n'
            'type="library"\nnative_abi=1\nlifecycle_abi=1\n' + dependencies +
            '\n[builtin]\npackage=' + json.dumps(package) + '\nentry="solar_os_engine_entry"\n')
        return path

    def test_manifest_is_the_only_identity_and_version_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = self.manifest(root)
            component, = load_builtin_components(root, {"backend_engine"})
            self.assertEqual((component.id, component.version, component.type), ("engine", "1.2.3", "library"))
            self.assertEqual(selected_components((component,), {"backend_engine": False}), ())
            path.write_text(path.read_text().replace('version="1.2.3"', 'version="1.2.4"'))
            self.assertEqual(load_builtin_components(root, {"backend_engine"})[0].version, "1.2.4")

    def test_unknown_package_invalid_entry_and_duplicate_identity_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = self.manifest(root)
            with self.assertRaisesRegex(ValueError, "unknown builtin package"):
                load_builtin_components(root, {})
            original = path.read_text()
            path.write_text(original.replace("solar_os_engine_entry", "entry();evil"))
            with self.assertRaisesRegex(ValueError, "invalid builtin entry"):
                load_builtin_components(root, {"backend_engine"})
            path.write_text(original)
            duplicate = root / "modules/duplicate/module.toml"
            duplicate.parent.mkdir()
            duplicate.write_text(original)
            with self.assertRaisesRegex(ValueError, "duplicate builtin identity"):
                load_builtin_components(root, {"backend_engine"})

    def test_compiled_provider_pins_and_cycles_are_validated(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.manifest(root)
            provider, = load_builtin_components(root, {"backend_engine"})
            consumer = dataclasses.replace(provider, id="client", type="app", dependencies=(("engine", "1.2.3"),))
            enabled = {"backend_engine": True}
            self.assertEqual(len(selected_components((provider, consumer), enabled)), 2)
            with self.assertRaisesRegex(ValueError, "needs engine"):
                selected_components((dataclasses.replace(provider, version="2.0.0"), consumer), enabled)
            cycle = dataclasses.replace(provider, dependencies=(("engine", "1.2.3"),))
            with self.assertRaisesRegex(ValueError, "dependency cycle"):
                selected_components((cycle,), enabled)

    def test_foreground_components_require_a_manifest_and_emit_its_reference(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = self.manifest(root, package="app_cc")
            source = path.read_text().replace('type="library"', 'type="app"').replace('lifecycle_abi=1', 'lifecycle_abi=2')
            path.write_text(source)
            with self.assertRaisesRegex(ValueError, "requires a manifest"):
                load_builtin_components(root, {"app_cc"})
            path.write_text(source + 'manifest="solar_os_engine_manifest"\n')
            component, = load_builtin_components(root, {"app_cc"})
            catalog = load_catalog(ROOT / "packages/solar_os_packages.toml")
            catalog = dataclasses.replace(catalog, components=(component,))
            flavor = ROOT / "flavors/full.toml"
            _, _, groups, enabled = load_flavor(flavor, catalog)
            header = generate_header("test", "", groups, enabled, catalog, flavor, ROOT / "packages/solar_os_packages.toml")
            self.assertIn("&solar_os_engine_manifest", header)
            self.assertIn("X(solar_os_engine_manifest)", header)
            self.assertIn("SOLAR_OS_NATIVE_FOREGROUND_ENABLED (SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES || 1)", header)
            (root / "solar_os_config.h").write_text(header)
            (root / "test.c").write_text('''#include <assert.h>
#include "solar_os_components.h"
const solar_os_native_app_manifest_v2_t solar_os_engine_manifest = {.id="engine"};
int solar_os_engine_entry(int n,char **v) { (void)v;return n; }
int main(void) {
    const solar_os_component_t *component=solar_os_component_find(SOLAR_OS_MODULE_TYPE_APP,"engine");
    assert(component && component->lifecycle_abi==2 && component->manifest==&solar_os_engine_manifest);
    return 0;
}
''')
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I" + str(root), "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src/services"),
                str(root / "test.c"), str(ROOT / "src/services/solar_os_components.c"), "-o", str(root / "test")], check=True)
            subprocess.run([str(root / "test")], check=True)
            path.write_text(source.replace('lifecycle_abi=2', 'lifecycle_abi=1') + 'manifest="solar_os_engine_manifest"\n')
            with self.assertRaisesRegex(ValueError, "only valid for a foreground"):
                load_builtin_components(root, {"app_cc"})

    def test_flavor_emits_manifest_versions_and_prunes_components_with_their_package(self):
        catalog = load_catalog(ROOT / "packages/solar_os_packages.toml")
        flavor = ROOT / "flavors/full.toml"
        _, _, groups, enabled = load_flavor(flavor, catalog)
        self.assertEqual({c.id for c in selected_components(catalog.components, enabled)}, {"cc", "tinycc"})
        classic = apply_target_pruning(catalog, enabled, "esp32")
        self.assertFalse(selected_components(catalog.components, classic))
        header = generate_header("test", "", groups, enabled, catalog, flavor, ROOT / "packages/solar_os_packages.toml")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "solar_os_config.h").write_text(header)
            source = root / "test.c"
            source.write_text('#include <assert.h>\n#include <string.h>\n#include "solar_os_components.h"\n'
                'int solar_os_cc_app_main(int n,char **v){(void)v;return n;}\n'
                'int solar_os_tinycc_app_main(int n,char **v){(void)v;return n;}\n'
                'int main(void){const solar_os_component_t *cc=solar_os_component_find(0,"cc");'
                'const solar_os_component_t *tcc=solar_os_component_find(0,"tinycc");'
                'assert(cc && tcc && cc->dependency_count==1);'
                'assert(!strcmp(cc->dependencies[0].id,tcc->id));'
                'assert(!strcmp(cc->dependencies[0].version,tcc->version));return 0;}\n')
            binary = root / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I" + str(root), "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src/services"), str(source),
                str(ROOT / "src/services/solar_os_components.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_registry_is_readonly_and_works_without_elf_or_storage(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "solar_os_config.h").write_text('''#define SOLAR_OS_PACKAGE_CORE_RUNTIME 1
#define SOLAR_OS_BUILTIN_COMPONENTS(X) \\
 X(SOLAR_OS_MODULE_TYPE_LIBRARY,"arithmetic","Arithmetic","1.2.3",1,solar_os_arithmetic,0,NULL,NULL) \\
 X(SOLAR_OS_MODULE_TYPE_JOB,"worker","Worker","2.0.0",1,solar_os_worker,0,NULL,NULL)
''')
            source = root / "test.c"
            source.write_text('''#include <assert.h>
#include <string.h>
#include "solar_os_components.h"
#include "solar_os_native_components.h"
#include "solar_os_native_components_abi.h"
int solar_os_arithmetic(int n, char **v) { (void)v; return n+1; }
int solar_os_worker(int n, char **v) { (void)v; return n+2; }
int main(void) {
 assert(solar_os_component_count()==2 && !solar_os_component_get(2));
 assert(!solar_os_component_find(0,0) && !solar_os_component_find(0,""));
 const solar_os_component_t *a=solar_os_component_find(SOLAR_OS_MODULE_TYPE_LIBRARY,"arithmetic");
 assert(a && !strcmp(a->version,"1.2.3") && a->entry(2,0)==3);
 assert(solar_os_component_find(0,"arithmetic")==a);
 assert(!solar_os_component_find(SOLAR_OS_MODULE_TYPE_JOB,"arithmetic"));
 assert(!solar_os_component_find(0,"unknown"));
 const solar_os_native_components_api_v1_t *api = solar_os_native_components_get_service("components",1,8);
 assert(api && api->count()==2);
 solar_os_native_component_info_v1_t info;
 assert(!api->info(0,&info) && !strcmp(info.id,"arithmetic") && !strcmp(info.version,"1.2.3"));
 assert(!api->find(SOLAR_OS_NATIVE_COMPONENT_JOB,"worker",&info) && info.type==SOLAR_OS_NATIVE_COMPONENT_JOB);
 assert(api->find(99,"worker",&info)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !info.id[0]);
 assert(api->find(0,0,&info)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
 assert(api->info(2,&info)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !info.id[0]);
 assert(api->info(0,0)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
 assert(!solar_os_native_components_get_service("components",2,8));
 assert(!solar_os_native_components_get_service("components",1,sizeof(*api)+1));
 return 0;
}
''')
            binary = root / "test"
            subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I" + str(root), "-I" + str(ROOT / "include"), "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src/services"), str(source),
                str(ROOT / "src/services/solar_os_components.c"), str(ROOT / "src/services/solar_os_native_components.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
