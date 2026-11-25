"""Core membership is independent of optional apps and board hardware choices."""
from pathlib import Path
import tempfile
import unittest
import sys
import subprocess

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from test_flavor_packages import generate_flavor_config as config
from flavor_config import SelectionModel, load_board_contexts

ROOT = Path(__file__).resolve().parents[1]


class CoreFlavorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalog = config.load_catalog(ROOT / "packages/solar_os_packages.toml")

    def test_every_service_has_explicit_core_ownership(self):
        for name, spec in self.catalog.package_defs.items():
            if name.startswith("service_"):
                self.assertTrue(spec.core, name)
            if name.startswith(("app_", "job_", "backend_")):
                self.assertFalse(spec.core, name)

    def test_core_has_only_bootstrap_apps_and_jobs(self):
        _, _, _, enabled = config.load_flavor(ROOT / "flavors/core.toml", self.catalog)
        optional = {name for name, value in enabled.items() if value and
                    name.startswith(("app_", "job_", "backend_"))}
        self.assertEqual(optional, {"app_ltop", "job_gesture_listener"})
        for name in ("app_git", "app_flash", "app_zoo", "app_python", "app_lua", "app_cc"):
            self.assertFalse(enabled[name], name)

    def test_classic_keeps_python_and_terminal_tools_with_a_smaller_app_selection(self):
        _, _, _, enabled = config.load_flavor(ROOT / "flavors/classic.toml", self.catalog)
        for name in ("app_python", "app_playground", "app_edit", "app_less", "app_files",
                     "app_ssh", "app_ftp", "app_io", "app_com"):
            self.assertTrue(enabled[name], name)
        for name in ("app_reader", "app_writer", "app_agent", "app_web", "app_chat",
                     "app_recorder", "app_player", "job_speechd", "job_httpd", "job_chatd"):
            self.assertFalse(enabled[name], name)
        for name, spec in self.catalog.package_defs.items():
            if spec.core:
                self.assertTrue(enabled[name], name)

    def test_services_are_identical_across_flavors_after_board_pruning(self):
        for board in load_board_contexts():
            baseline = None
            for flavor in ("core", "writerdeck", "classic", "full"):
                _, _, groups, enabled = config.load_flavor(ROOT / "flavors" / (flavor + ".toml"), self.catalog)
                enabled = config.enable_required_packages(self.catalog, enabled, set(board.required_packages))
                enabled = config.apply_update_layout(self.catalog, enabled, "ota")
                enabled = config.apply_target_pruning(self.catalog, enabled, board.target)
                _, enabled = config.apply_board_capability_pruning(self.catalog, groups, enabled, set(board.capabilities))
                services = {name for name, value in enabled.items() if value and self.catalog.package_defs[name].core}
                if baseline is None:
                    baseline = services
                self.assertEqual(services, baseline, (board.board_id, flavor))

    def test_core_services_cannot_be_disabled_by_flavor_or_configurator(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "invalid.toml"
            path.write_text('[flavor]\nname="invalid"\n[packages]\nservice_audio=false\n')
            with self.assertRaisesRegex(ValueError, "cannot be disabled.*service_audio"):
                config.load_flavor(path, self.catalog)
        model = SelectionModel(self.catalog)
        self.assertIn("service_audio", model.mandatory)
        model.toggle_group(("service_audio",))
        self.assertIn("service_audio", model.selected)

    def test_absent_hid_hardware_reports_unsupported(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "hid.c"
            source.write_text('#include <assert.h>\n#include "hid_port.h"\n'
                'int main(void) { assert(!hid_port_available()); '
                'assert(!hid_port_is_connected()); '
                'assert(hid_port_send_report(1, 0, 0) == ESP_ERR_NOT_SUPPORTED); return 0; }\n')
            binary = Path(temporary) / "hid"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "tests/host"), "-I" + str(ROOT / "src/drivers"),
                str(source), str(ROOT / "src/drivers/hid_port_unavailable.c"),
                "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_single_layout_prunes_ota_dependants_consistently(self):
        _, _, _, enabled = config.load_flavor(ROOT / "flavors/core.toml", self.catalog)
        enabled = config.apply_update_layout(self.catalog, enabled, "single")
        model = SelectionModel(self.catalog, layout="single")
        self.assertEqual({name for name, value in enabled.items() if value}, model.selected)
        self.assertFalse(enabled["service_ota"])
        self.assertFalse(enabled["service_docs"])
        self.assertTrue(enabled["service_system_api"])


if __name__ == "__main__":
    unittest.main()
