import ast
import re
import runpy
import sys
import tempfile
import tomllib
import types
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import generate_flavor_config as flavors
import patch_espdl as overlay


class InferenceIntegrationTest(unittest.TestCase):
    def test_platformio_overlay_runs_for_cached_inference_builds(self):
        class Environment:
            def subst(self, value):
                return str(component_root if value == "$PROJECT_DIR" else build_dir)

        with tempfile.TemporaryDirectory() as directory:
            component_root = Path(directory)
            build_dir = component_root / ".pio/build/fixture"
            config = build_dir / "generated/solar_os/solar_os_config.h"
            config.parent.mkdir(parents=True)
            config.write_text("#define SOLAR_OS_PACKAGE_SERVICE_INFERENCE 1\n")
            scons = types.ModuleType("SCons.Script")
            scons.GetOption = lambda name: False
            script = ROOT / "scripts/platformio_espdl.py"
            with patch.dict(sys.modules, {"SCons.Script": scons}), patch.object(overlay, "patch") as apply:
                for _ in range(2):
                    runpy.run_path(str(script), init_globals={"env": Environment(), "Import": lambda name: None})
                self.assertEqual(apply.call_count, 2)
                apply.assert_called_with(component_root / "managed_components/espressif__esp-dl")
                config.write_text("#define SOLAR_OS_PACKAGE_SERVICE_INFERENCE 0\n")
                runpy.run_path(str(script), init_globals={"env": Environment(), "Import": lambda name: None})
                self.assertEqual(apply.call_count, 2)
                config.unlink()
                scons.GetOption = lambda name: True
                runpy.run_path(str(script), init_globals={"env": Environment(), "Import": lambda name: None})
                self.assertEqual(apply.call_count, 2)

    def test_worker_overlay_is_repeatable_and_rejects_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            component = Path(directory)
            source = component / "dl/module/src/dl_module_base.cpp"
            header = component / "dl/module/include/dl_module_base.hpp"
            source.parent.mkdir(parents=True)
            header.parent.mkdir(parents=True)
            original = (ROOT / "tests/fixtures/inference/worker_base.cpp").read_text()
            source.write_text(original)
            header.write_text("void module_forward_dual_core(Module *op, void *args1, void *args2);")
            overlay.patch_workers(component)
            first = source.read_text()
            overlay.patch_workers(component)
            self.assertEqual(source.read_text(), first)
            self.assertEqual(source.with_name("solar_os_dl_workers.inc").read_bytes(),
                (ROOT / "scripts/espdl/worker_runtime.inc").read_bytes())
            self.assertIn("void module_workers_release();", header.read_text())
            header.write_text("void module_forward_dual_core(Module *op, void *args1, void *args2);")
            overlay.patch_workers(component)
            self.assertIn("void module_workers_release();", header.read_text())
            source.write_text(original.replace("kWorkerStackBytes = 2048", "kWorkerStackBytes = 4096"))
            with self.assertRaisesRegex(ValueError, "review overlay"):
                overlay.patch_workers(component)

    def test_kernel_placement_preserves_instructions_and_other_targets(self):
        with tempfile.TemporaryDirectory() as directory:
            component = Path(directory)
            source = component / "dl/base/isa/tie728/kernel.S"
            other = component / "dl/base/isa/other/kernel.S"
            for path in (source, other):
                path.parent.mkdir(parents=True)
                path.write_text("    .section .iram1\n    EE.ZERO.QACC\n    # .section .iram1\n")
            original = source.read_text()
            overlay.patch_flash_kernels(component)
            self.assertIn("EE.ZERO.QACC", source.read_text())
            self.assertIn("# .section .iram1", source.read_text())
            self.assertEqual(other.read_text(), original)
            first = source.read_text()
            overlay.patch_flash_kernels(component)
            self.assertEqual(source.read_text(), first)
            overlay.patch_flash_kernels(component, "iram")
            self.assertEqual(source.read_text(), original)

    def test_target_and_psram_gates_without_camera(self):
        catalog = flavors.load_catalog(ROOT / "packages/solar_os_packages.toml")
        groups = {group: group == "inference" for group in catalog.groups}
        selected = flavors.enable_required_packages(catalog,
            {name: False for name in catalog.packages}, {"service_inference"})
        for target in ("esp32", "esp32s3", "esp32c3", "esp32p4"):
            for capabilities in (set(), {"psram", "wifi"}):
                _, packages = flavors.apply_board_capability_pruning(catalog, groups,
                    flavors.apply_target_pruning(catalog, selected, target), capabilities)
                self.assertEqual(packages["service_inference"], target == "esp32s3" and bool(capabilities))
                for dependency in ("service_camera", "service_image", "service_vision"):
                    self.assertFalse(packages[dependency])
        for name in ("writerdeck", "full"):
            flavor = tomllib.loads((ROOT / f"flavors/{name}.toml").read_text())
            self.assertTrue(flavor["groups"].get("inference", False))
            self.assertFalse(flavor["groups"].get("inference_backend", False))

    def test_builtin_backend_remains_selectable_for_custom_profiles(self):
        catalog = flavors.load_catalog(ROOT / "packages/solar_os_packages.toml")
        groups = {name: name in ("inference", "inference_backend") for name in catalog.groups}
        selected = flavors.enable_required_packages(catalog,
            dict.fromkeys(catalog.packages, False), {"backend_inference_espdl"})
        _, packages = flavors.apply_board_capability_pruning(catalog, groups,
            flavors.apply_target_pruning(catalog, selected, "esp32s3"), {"psram", "wifi"})
        self.assertTrue(packages["backend_inference_espdl"])
        self.assertTrue(packages["service_inference"])
        self.assertIn("esp-dl", flavors.collect_requires(catalog, groups, packages))
        self.assertIn("services/solar_os_inference_espdl.cpp",
                      flavors.collect_sources(catalog, groups, packages))

    def test_documented_python_examples_parse(self):
        for example in re.findall(r"```python\n(.*?)```", (ROOT / "doc/manual/inference.md").read_text(), re.S):
            ast.parse(example)

    def test_overlay_rejects_version_and_source_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            component = Path(directory)
            manifest = component / "idf_component.yml"
            manifest.write_text('version: "3.3.12"\n')
            with self.assertRaisesRegex(ValueError, "pinned version"):
                overlay.patch(component)
            manifest.write_text('version: 3.3.13\n')
            header = component / "dl/model/include/dl_model_base.hpp"
            header.parent.mkdir(parents=True)
            header.write_text("    Model() { unexpected(); }\n")
            with self.assertRaisesRegex(ValueError, "review overlay"):
                overlay.patch(component)
            header.write_text(overlay.OLD)
            overlay.replace(header, overlay.OLD, overlay.NEW)
            overlay.replace(header, overlay.OLD, overlay.NEW)
            self.assertEqual(header.read_text(), overlay.NEW)


if __name__ == "__main__":
    unittest.main()
