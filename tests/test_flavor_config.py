import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
SCRIPTS = REPOSITORY / "scripts"
sys.path.insert(0, str(SCRIPTS))

GENERATOR_SPEC = importlib.util.spec_from_file_location(
    "generate_flavor_config", SCRIPTS / "generate_flavor_config.py"
)
generate_flavor_config = importlib.util.module_from_spec(GENERATOR_SPEC)
sys.modules[GENERATOR_SPEC.name] = generate_flavor_config
GENERATOR_SPEC.loader.exec_module(generate_flavor_config)

CONFIG_SPEC = importlib.util.spec_from_file_location(
    "flavor_config", SCRIPTS / "flavor_config.py"
)
flavor_config = importlib.util.module_from_spec(CONFIG_SPEC)
sys.modules[CONFIG_SPEC.name] = flavor_config
CONFIG_SPEC.loader.exec_module(flavor_config)

from solaros_update_layout import select_layout


class Window:
    def keypad(self, _enabled):
        pass


class FlavorConfigTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalog = generate_flavor_config.load_catalog(
            REPOSITORY / "packages" / "solar_os_packages.toml"
        )
        cls.boards = {
            board.board_id: board for board in flavor_config.load_board_contexts()
        }

    def test_visible_choices_are_granular_groups(self):
        groups = flavor_config.visible_groups(self.catalog)
        names = {
            name for name in self.catalog.groups
            if not self.catalog.group_defs[name].hidden
        }
        self.assertNotIn("bootstrap", names)
        self.assertIn("ssh", names)
        self.assertIn("http_client", names)
        self.assertIn("ftp", names)
        self.assertIn("meshcore", names)
        self.assertIn("gameboy", names)
        self.assertNotIn("ota", names)
        self.assertNotIn("usb_hid", names)
        self.assertEqual(len(groups), len(names))
        self.assertEqual(self.catalog.group_defs["ssh"].members,
                         ("app_ssh", "app_scp"))
        self.assertEqual(self.catalog.group_defs["ssh"].category, "Networking")

    def test_tree_contains_categories_and_groups_but_no_packages(self):
        estimator = flavor_config.FlashEstimator(self.catalog, {}, {}, "test")
        screen = flavor_config.FlavorScreen(
            Window(), self.catalog, flavor_config.SelectionModel(self.catalog),
            estimator, None, Path("input.toml"), Path("output.toml"),
        )
        screen.expanded.update(category for category, _ in screen.categories)
        rows = screen.rows()
        self.assertTrue(any(row.kind == "category" for row in rows))
        self.assertTrue(any(row.kind == "group" and row.name == "ssh" for row in rows))
        self.assertEqual({row.kind for row in rows}, {"category", "group"})
        self.assertFalse(any(row.name == "service_ssh" for row in rows))

    def test_group_toggle_resolves_hidden_dependencies(self):
        model = flavor_config.SelectionModel(self.catalog)
        ssh = self.catalog.group_defs["ssh"]
        model.toggle_group(ssh.members)
        self.assertEqual(model.group_state(ssh.members), 2)
        self.assertIn("app_ssh", model.selected)
        self.assertIn("app_scp", model.selected)
        self.assertIn("service_ssh", model.selected)
        self.assertIn("service_wifi", model.selected)
        model.toggle_group(ssh.members)
        self.assertNotIn("app_ssh", model.selected)

    def test_board_context_uses_partition_and_required_drivers(self):
        solar_term = self.boards["solar_term"]
        self.assertEqual(solar_term.flash_bytes, 16 * 1024 * 1024)
        self.assertEqual(solar_term.app_bytes, 0x700000)
        self.assertEqual(solar_term.psram_bytes, 8 * 1024 * 1024)
        self.assertIn("driver_display_st7305", solar_term.required_packages)
        self.assertIn("driver_audio_es8311_codecs", solar_term.required_packages)

        elecrow = self.boards["elecrow_crowpanel_esp32_s3_4_2_epaper"]
        self.assertEqual(elecrow.flash_bytes, 8 * 1024 * 1024)
        self.assertEqual(elecrow.app_bytes, 0x3E0000)

        rover = self.boards["freenove_esp32_wrover_v3"]
        self.assertEqual(rover.flash_bytes, 4 * 1024 * 1024)
        self.assertEqual(rover.app_bytes, 0x3D0000)

    def test_update_layouts_follow_board_flash_capacity(self):
        solar_term = self.boards["solar_term"]
        self.assertEqual(
            [(layout.key, layout.app_bytes) for layout in solar_term.layouts],
            [("ota", 0x700000), ("single", 0xE00000)],
        )
        elecrow = self.boards["elecrow_crowpanel_esp32_s3_4_2_epaper"]
        self.assertEqual(
            [(layout.key, layout.app_bytes) for layout in elecrow.layouts],
            [("ota", 0x3E0000), ("single", 0x700000)],
        )
        rover = self.boards["freenove_esp32_wrover_v3"]
        self.assertEqual(
            [(layout.key, layout.app_bytes) for layout in rover.layouts],
            [("single", 0x3D0000)],
        )

    def test_platformio_layout_selection_uses_the_same_layout_model(self):
        self.assertEqual(select_layout("partitions.csv").key, "ota")
        self.assertEqual(select_layout("partitions_8mb.csv", "single").app_bytes,
                         0x700000)
        self.assertEqual(select_layout("partitions_4mb.csv").key, "single")
        with self.assertRaisesRegex(ValueError, "not available"):
            select_layout("partitions_4mb.csv", "ota")

        for board in self.boards.values():
            for layout in board.layouts:
                with self.subTest(board=board.board_id, layout=layout.key):
                    flash_bytes, app_bytes = flavor_config._partition_limits(
                        REPOSITORY / layout.partition_file
                    )
                    self.assertEqual(flash_bytes, board.flash_bytes)
                    self.assertEqual(app_bytes, layout.app_bytes)

    def test_ota_is_owned_by_layout_and_not_saved_as_a_group(self):
        ota = flavor_config.SelectionModel(self.catalog, (), layout="ota")
        single = flavor_config.SelectionModel(
            self.catalog, {"service_ota"}, layout="single"
        )
        self.assertIn("service_ota", ota.mandatory)
        self.assertNotIn("service_ota", single.selected)
        content = flavor_config.render_flavor("test", "test", self.catalog, ota)
        self.assertNotIn("ota = true", content)
        self.assertNotIn("service_ota", content)

    def test_build_identity_includes_board_layout_and_flavor(self):
        board = self.boards["solar_term"]
        ota, single = board.layouts
        first = flavor_config.build_fingerprint(board, ota, "one")
        self.assertEqual(first, flavor_config.build_fingerprint(board, ota, "one"))
        self.assertNotEqual(first, flavor_config.build_fingerprint(board, single, "one"))
        self.assertNotEqual(first, flavor_config.build_fingerprint(board, ota, "two"))

    def test_selection_change_marks_successful_build_stale(self):
        board = self.boards["solar_term"]
        layout = board.layouts[0]
        model = flavor_config.SelectionModel(
            self.catalog,
            (),
            board.required_packages,
            board.target,
            board.capabilities,
            layout.key,
        )
        screen = flavor_config.FlavorScreen(
            Window(), self.catalog, model,
            flavor_config.FlashEstimator(self.catalog, {}, {}, "test"),
            board, Path("input.toml"), Path("output.toml"), layout,
        )
        screen.built_fingerprint = screen._fingerprint()
        screen.measured_size = 1234
        self.assertTrue(screen.build_is_current)
        model.toggle_group(self.catalog.group_defs["ssh"].members)
        self.assertFalse(screen.build_is_current)

    def test_builder_command_and_environment_are_explicit(self):
        board = self.boards["solar_term"]
        layout = board.layouts[1]
        command = flavor_config.builder_command(board, True, "/dev/ttyUSB7")
        self.assertEqual(command[-4:], ["-t", "upload", "--upload-port", "/dev/ttyUSB7"])
        environment = flavor_config.builder_environment(
            board, layout, REPOSITORY / ".pio" / "os_builder" / "current.toml"
        )
        self.assertEqual(environment["SOLAR_OS_BOARD"], board.board_id)
        self.assertEqual(environment["SOLAR_OS_LAYOUT"], "single")
        self.assertTrue(environment["SOLAR_OS_FLAVOR_FILE"].endswith("current.toml"))

    def test_progress_parser_handles_build_and_flash_output(self):
        self.assertEqual(
            flavor_config.progress_for_line("build", "Linking .pio/build/fw.elf", 20),
            (84, "Linking firmware"),
        )
        self.assertEqual(
            flavor_config.progress_for_line(
                "flash", "Writing at 0x00010000... (50 %)", 20
            ),
            (55, "Writing flash"),
        )

    def test_board_required_group_is_locked_and_services_are_automatic(self):
        board = self.boards["solar_term"]
        model = flavor_config.SelectionModel(
            self.catalog, (), board.required_packages, board.target, board.capabilities
        )
        display = self.catalog.group_defs["st7305"]
        self.assertTrue(model.group_locked(display))
        self.assertIn("driver_display_st7305", model.selected)
        self.assertIn("service_spi", model.selected)

    def test_all_flavors_resolve_for_every_board_context(self):
        for flavor_path in sorted((REPOSITORY / "flavors").glob("*.toml")):
            requested = flavor_config.load_requested_packages(flavor_path, self.catalog)
            for board in self.boards.values():
                with self.subTest(flavor=flavor_path.stem, board=board.board_id):
                    model = flavor_config.SelectionModel(
                        self.catalog,
                        requested,
                        board.required_packages,
                        board.target,
                        board.capabilities,
                    )
                    self.assertLessEqual(board.required_packages, model.selected)

    def test_unsupported_group_reports_board_capability(self):
        board = self.boards["esp32_s3_devkitc1_n16r8"]
        model = flavor_config.SelectionModel(
            self.catalog, (), board.required_packages, board.target, board.capabilities
        )
        gameboy = self.catalog.group_defs["gameboy"]
        self.assertFalse(model.group_supported(gameboy))
        self.assertIn("requires", model.unsupported_reason(gameboy))
        self.assertIn("streaming_display", model.unsupported_reason(gameboy))

    def test_rendered_flavor_uses_groups_and_round_trips(self):
        members = set(self.catalog.group_defs["ssh"].members)
        members.update(self.catalog.group_defs["http_client"].members)
        model = flavor_config.SelectionModel(self.catalog, members)
        content = flavor_config.render_flavor(
            "test-custom", "Generated by a test.", self.catalog, model
        )
        self.assertIn("[groups]", content)
        self.assertIn("ssh = true", content)
        self.assertIn("http_client = true", content)
        self.assertNotIn("service_ssh", content)
        self.assertNotIn("[packages]", content)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test-custom.toml"
            path.write_text(content, encoding="utf-8")
            _, _, _, enabled = generate_flavor_config.load_flavor(path, self.catalog)
        resolved = {package for package, value in enabled.items() if value}
        self.assertEqual(resolved, model.selected)

    def test_board_requirements_do_not_leak_into_portable_flavor(self):
        board = self.boards["solar_term"]
        model = flavor_config.SelectionModel(
            self.catalog, (), board.required_packages, board.target, board.capabilities
        )
        content = flavor_config.render_flavor(
            "board-only", "No optional groups.", self.catalog, model
        )
        self.assertNotIn("st7305 = true", content)
        self.assertNotIn("es8311 = true", content)
        self.assertNotIn("sdmmc = true", content)

    def test_explicit_board_driver_remains_portable_when_saved(self):
        board = self.boards["solar_term"]
        st7305 = self.catalog.group_defs["st7305"]
        model = flavor_config.SelectionModel(
            self.catalog,
            st7305.members,
            board.required_packages,
            board.target,
            board.capabilities,
        )
        content = flavor_config.render_flavor(
            "display", "Portable display selection.", self.catalog, model
        )
        self.assertIn("st7305 = true", content)

    def test_advanced_package_override_remains_available(self):
        model = flavor_config.SelectionModel(self.catalog, {"service_json_scan"})
        content = flavor_config.render_flavor(
            "advanced", "Advanced override.", self.catalog, model
        )
        self.assertIn("[packages]", content)
        self.assertIn("service_json_scan = true", content)

    def test_generated_configs_list_enabled_package_ids(self):
        groups = {group: False for group in self.catalog.groups}
        packages = {package: False for package in self.catalog.packages}
        packages["service_wifi"] = True
        packages["app_help"] = True
        expected = " ".join(
            package for package in self.catalog.packages if packages[package]
        )
        source = Path("test.toml")
        package_catalog = Path("packages.toml")
        header = generate_flavor_config.generate_header(
            "test", "test", groups, packages, self.catalog,
            source, package_catalog,
        )
        cmake = generate_flavor_config.generate_cmake(
            "test", "test", groups, packages, self.catalog,
            source, package_catalog,
        )
        self.assertIn(
            f'#define SOLAR_OS_PACKAGE_ID_LIST "{expected}"', header
        )
        self.assertIn(
            f'set(SOLAR_OS_PACKAGE_ID_LIST "{expected}")', cmake
        )

    def test_catalog_rejects_package_unreachable_from_every_group(self):
        content = (
            REPOSITORY / "packages" / "solar_os_packages.toml"
        ).read_text(encoding="utf-8")
        content += "\n[packages.test_orphan]\nlabel = \"test.orphan\"\n"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "packages.toml"
            path.write_text(content, encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "not reachable from any group"):
                generate_flavor_config.load_catalog(path)

    def test_estimator_counts_shared_sources_and_requirements_once(self):
        estimator = flavor_config.FlashEstimator(
            self.catalog,
            {source: 10 for package in self.catalog.package_defs.values()
             for source in package.sources},
            {requirement: 100 for package in self.catalog.package_defs.values()
             for requirement in package.requires},
            "test",
        )
        packages = {"app_aplay", "app_arecord"}
        sources = {
            source for package in packages
            for source in self.catalog.package_defs[package].sources
        }
        requirements = {
            requirement for package in packages
            for requirement in self.catalog.package_defs[package].requires
        }
        self.assertEqual(
            estimator.estimate(packages),
            10 * len(sources) + 100 * len(requirements),
        )


if __name__ == "__main__":
    unittest.main()
