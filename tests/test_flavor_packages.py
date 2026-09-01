import importlib.util
from pathlib import Path
import sys
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
GENERATOR_PATH = REPOSITORY / "scripts" / "generate_flavor_config.py"
SPEC = importlib.util.spec_from_file_location("generate_flavor_config", GENERATOR_PATH)
assert SPEC is not None and SPEC.loader is not None
generate_flavor_config = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = generate_flavor_config
SPEC.loader.exec_module(generate_flavor_config)


class FlavorPackagesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalog = generate_flavor_config.load_catalog(
            REPOSITORY / "packages" / "solar_os_packages.toml"
        )

    def resolve(self, flavor):
        return generate_flavor_config.load_flavor(
            REPOSITORY / "flavors" / f"{flavor}.toml",
            self.catalog,
        )

    def test_games_are_in_full_beta_and_rover_gameboy(self):
        for flavor_path in sorted((REPOSITORY / "flavors").glob("*.toml")):
            _, _, groups, packages = self.resolve(flavor_path.stem)
            expected = flavor_path.stem in {"full", "beta", "rover-gameboy"}
            self.assertEqual(groups["games"], expected, flavor_path.stem)
            invaders_expected = flavor_path.stem in {"full", "beta"}
            self.assertEqual(packages["app_invaders"], invaders_expected,
                             flavor_path.stem)
            self.assertEqual(packages["app_gameboy"], expected,
                             flavor_path.stem)

    def test_sketch_is_media_without_pointer_or_psram_gates(self):
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["media"])
        self.assertTrue(packages["app_sketch"])

        pruned_groups, pruned_packages = (
            generate_flavor_config.apply_board_capability_pruning(
                self.catalog,
                groups,
                packages,
                {"gfx"},
            )
        )
        self.assertTrue(pruned_groups["media"])
        self.assertTrue(pruned_packages["app_sketch"])
        self.assertFalse(pruned_packages["app_view"])

    def test_board_required_package_enables_dependencies(self):
        _, _, _, packages = self.resolve("core")
        enabled = generate_flavor_config.enable_required_packages(
            self.catalog,
            packages,
            {"job_ps2_keyboard"},
        )

        self.assertTrue(enabled["job_ps2_keyboard"])
        self.assertTrue(enabled["service_ps2"])
        self.assertTrue(enabled["service_resources"])

    def test_unknown_board_required_package_is_rejected(self):
        _, _, _, packages = self.resolve("core")
        with self.assertRaisesRegex(ValueError, "unknown board-required package"):
            generate_flavor_config.enable_required_packages(
                self.catalog,
                packages,
                {"job_missing"},
            )

    def test_target_pruning_keeps_compatible_driver_packages(self):
        _, _, _, packages = self.resolve("full")

        for target in ("esp32", "esp32s3"):
            pruned = generate_flavor_config.apply_target_pruning(
                self.catalog,
                packages,
                target,
            )
            self.assertTrue(pruned["expansion_neopixel"], target)
            self.assertTrue(pruned["expansion_audio_pwm"], target)
            self.assertTrue(pruned["expansion_pcm1808"], target)
            self.assertTrue(pruned["expansion_pcm5102"], target)
            self.assertTrue(pruned["driver_pcf85063"], target)
            self.assertTrue(pruned["driver_shtc3"], target)
            self.assertTrue(pruned["driver_battery_adc"], target)
            self.assertTrue(pruned["expansion_sdmmc"], target)

    def test_target_pruning_removes_incompatible_driver_and_dependents(self):
        _, _, _, packages = self.resolve("full")
        pruned = generate_flavor_config.apply_target_pruning(
            self.catalog,
            packages,
            "esp32c3",
        )

        self.assertFalse(pruned["expansion_neopixel"])
        self.assertFalse(pruned["expansion_audio_pwm"])
        self.assertFalse(pruned["expansion_pcm1808"])
        self.assertFalse(pruned["expansion_pcm5102"])

    def test_sdmmc_expansion_uses_direct_gpio_capability(self):
        _, _, groups, packages = self.resolve("full")
        _, pruned = generate_flavor_config.apply_board_capability_pruning(
            self.catalog,
            groups,
            packages,
            {"expansion_gpio"},
        )

        self.assertTrue(pruned["expansion_sdmmc"])
        self.assertFalse(pruned["expansion_sdspi"])

    def test_audio_backend_drivers_are_target_specific(self):
        _, _, _, packages = self.resolve("full")

        classic = generate_flavor_config.apply_target_pruning(
            self.catalog,
            packages,
            "esp32",
        )
        self.assertTrue(classic["driver_audio_esp32_dac"])
        self.assertFalse(classic["driver_audio_es8311_codecs"])

        s3 = generate_flavor_config.apply_target_pruning(
            self.catalog,
            packages,
            "esp32s3",
        )
        self.assertFalse(s3["driver_audio_esp32_dac"])
        self.assertTrue(s3["driver_audio_es8311_codecs"])

    def test_spi_display_drivers_support_both_esp32_targets(self):
        _, _, _, packages = self.resolve("full")

        portable_spi_displays = (
            "driver_display_st7305",
            "driver_display_ili9341",
            "driver_display_st7796",
            "expansion_ssd1683",
        )

        classic = generate_flavor_config.apply_target_pruning(
            self.catalog,
            packages,
            "esp32",
        )
        for package in portable_spi_displays:
            self.assertTrue(classic[package], package)
        self.assertTrue(classic["driver_display_cvbs_pal"])
        self.assertTrue(classic["driver_display_vga32"])

        s3 = generate_flavor_config.apply_target_pruning(
            self.catalog,
            packages,
            "esp32s3",
        )
        for package in portable_spi_displays:
            self.assertTrue(s3[package], package)
        self.assertFalse(s3["driver_display_cvbs_pal"])
        self.assertFalse(s3["driver_display_vga32"])

    def test_target_pruning_requires_a_target(self):
        _, _, _, packages = self.resolve("full")
        with self.assertRaisesRegex(ValueError, "MCU target is required"):
            generate_flavor_config.apply_target_pruning(
                self.catalog,
                packages,
                "",
            )

    def test_granular_group_ownership(self):
        self.assertEqual(
            set(self.catalog.group_defs["maintenance_jobs"].members),
            {"job_log", "job_batmon"},
        )
        self.assertEqual(
            set(self.catalog.group_defs["hardware_jobs"].members),
            {"job_bridge", "job_daq", "job_gpio_keys", "job_ps2_keyboard", "job_sump"},
        )
        self.assertEqual(
            set(self.catalog.group_defs["writing"].members),
            {"app_reader", "app_writer", "app_files", "app_notes"},
        )
        self.assertEqual(
            set(self.catalog.group_defs["utils"].members),
            {"app_clock", "app_calc", "app_plot", "app_logic", "app_sheet"},
        )
        self.assertEqual(
            set(self.catalog.group_defs["games"].members),
            {"app_invaders", "app_gameboy"},
        )
        self.assertIn("service_synth", self.catalog.group_defs["system"].members)
        self.assertIn("service_streams", self.catalog.group_defs["system"].members)
        self.assertIn("job_controls", self.catalog.group_defs["system"].members)
        self.assertIn("service_synth", self.catalog.group_defs["audio"].members)
        self.assertEqual(
            self.catalog.group_defs["experimental"].members,
            ("service_hid",),
        )
        self.assertEqual(
            self.catalog.package_defs["service_synth"].depends,
            ("service_audio", "service_dsp", "service_streams"),
        )
        self.assertEqual(
            self.catalog.package_defs["core_runtime"].depends,
            ("service_streams",),
        )
        self.assertEqual(
            self.catalog.package_defs["service_audio"].depends,
            ("service_streams",),
        )
        self.assertEqual(
            self.catalog.package_defs["service_audio"].capabilities,
            (),
        )
        self.assertEqual(
            self.catalog.package_defs["service_audio_codecs"].requires,
            ("minimp3",),
        )
        self.assertEqual(
            self.catalog.package_defs["app_aplay"].depends,
            ("service_audio", "service_audio_codecs"),
        )
        self.assertEqual(
            self.catalog.package_defs["service_webradio"].requires,
            ("nvs_flash",),
        )
        self.assertEqual(
            self.catalog.package_defs["app_webradio"].depends,
            (
                "service_audio",
                "service_audio_codecs",
                "service_http_client",
                "service_media_widgets",
                "service_signal_widgets",
                "service_webradio",
            ),
        )
        self.assertEqual(
            self.catalog.package_defs["app_webradio"].capabilities,
            ("wifi",),
        )
        self.assertEqual(self.catalog.package_defs["app_aplay"].capabilities, ())
        self.assertEqual(self.catalog.package_defs["app_arecord"].capabilities, ())
        self.assertEqual(
            self.catalog.package_defs["app_recorder"].depends,
            (
                "service_audio",
                "service_media_widgets",
                "service_signal_widgets",
                "service_storage_browser",
            ),
        )
        self.assertEqual(self.catalog.package_defs["app_recorder"].capabilities, ())
        self.assertEqual(
            self.catalog.package_defs["app_player"].depends,
            (
                "service_audio",
                "service_audio_codecs",
                "service_media_widgets",
                "service_player_playlist",
                "service_signal_widgets",
                "service_storage_browser",
            ),
        )
        self.assertEqual(self.catalog.package_defs["app_player"].capabilities, ())
        self.assertEqual(
            self.catalog.package_defs["expansion_audio_pwm"].depends,
            ("service_audio", "service_expansion"),
        )
        self.assertEqual(
            self.catalog.package_defs["expansion_audio_pwm"].capabilities,
            ("expansion_pwm",),
        )
        self.assertEqual(
            self.catalog.package_defs["expansion_pcm1808"].depends,
            ("service_audio", "service_expansion"),
        )
        self.assertEqual(
            self.catalog.package_defs["expansion_pcm1808"].capabilities,
            ("expansion_i2s",),
        )
        self.assertEqual(
            self.catalog.package_defs["expansion_pcm5102"].depends,
            ("service_audio", "service_expansion"),
        )
        self.assertEqual(
            self.catalog.package_defs["expansion_pcm5102"].capabilities,
            ("expansion_i2s",),
        )
        self.assertEqual(
            self.catalog.package_defs["expansion_ssd1683"].depends,
            ("service_expansion", "service_spi"),
        )
        self.assertEqual(
            self.catalog.package_defs["expansion_ssd1683"].capabilities,
            ("gfx", "expansion_gpio"),
        )
        self.assertEqual(
            self.catalog.package_defs["service_espnow"].depends,
            ("service_wifi",),
        )
        self.assertIn("service_wireguard", self.catalog.group_defs["net"].members)
        self.assertIn("service_osc", self.catalog.group_defs["net"].members)
        self.assertIn("job_osc", self.catalog.group_defs["net"].members)
        self.assertEqual(
            self.catalog.package_defs["service_osc"].depends,
            ("service_controls", "service_streams"),
        )
        self.assertEqual(
            self.catalog.package_defs["job_osc"].depends,
            ("service_net", "service_osc"),
        )
        self.assertEqual(
            self.catalog.package_defs["service_wireguard"].depends,
            ("service_wifi",),
        )
        self.assertEqual(
            self.catalog.package_defs["service_wireguard"].capabilities,
            ("wifi",),
        )
        self.assertIn(
            "wireguard_lwip",
            self.catalog.package_defs["service_wireguard"].requires,
        )
        self.assertEqual(
            self.catalog.package_defs["job_espnow_link"].depends,
            ("service_espnow", "service_inbox", "service_link"),
        )
        self.assertEqual(
            self.catalog.package_defs["job_espnow_link"].capabilities,
            ("wifi",),
        )
        self.assertEqual(
            self.catalog.package_defs["app_gameboy"].depends,
            (),
        )

    def test_writerdeck_selects_writing_without_hardware_jobs_or_utils(self):
        name, _, groups, packages = self.resolve("writerdeck")

        self.assertEqual(name, "writerdeck")
        self.assertTrue(groups["writing"])
        self.assertTrue(groups["maintenance_jobs"])
        self.assertFalse(groups["hardware_jobs"])
        self.assertFalse(groups["utils"])
        for package in ("app_reader", "app_writer", "app_files", "app_notes", "job_log"):
            self.assertTrue(packages[package], package)
        self.assertTrue(packages["job_controls"])
        for package in (
            "job_bridge",
            "job_daq",
            "job_sump",
            "service_script_net",
            "app_python",
            "app_lua",
            "app_clock",
            "app_calc",
            "app_plot",
            "app_logic",
            "app_sheet",
        ):
            self.assertFalse(packages[package], package)

    def test_audio_apps_and_codecs_survive_without_board_audio(self):
        _, _, groups, packages = self.resolve("full")
        _, pruned = generate_flavor_config.apply_board_capability_pruning(
            self.catalog,
            groups,
            packages,
            set(),
        )

        for package in (
            "service_audio",
            "service_audio_codecs",
            "service_synth",
            "app_aplay",
            "app_arecord",
            "app_recorder",
            "app_player",
            "app_synth",
            "app_funcgen",
        ):
            self.assertTrue(pruned[package], package)
        self.assertFalse(pruned["service_espnow"])
        self.assertFalse(pruned["service_wireguard"])
        self.assertFalse(pruned["job_espnow_link"])

    def test_full_does_not_select_dormant_hid(self):
        _, _, groups, packages = self.resolve("full")
        self.assertFalse(groups["experimental"])
        self.assertFalse(packages["service_hid"])

    def test_webradio_survives_on_wifi_board_without_builtin_audio(self):
        _, _, groups, packages = self.resolve("full")
        _, pruned = generate_flavor_config.apply_board_capability_pruning(
            self.catalog,
            groups,
            packages,
            {"wifi"},
        )

        for package in (
            "service_audio",
            "service_audio_codecs",
            "service_synth",
            "service_http_client",
            "service_signal_widgets",
            "service_webradio",
            "app_synth",
            "app_funcgen",
            "app_webradio",
            "service_espnow",
            "service_wireguard",
            "job_espnow_link",
        ):
            self.assertTrue(pruned[package], package)

    def test_pwm_audio_expansion_survives_without_builtin_audio(self):
        _, _, groups, packages = self.resolve("full")
        _, pruned = generate_flavor_config.apply_board_capability_pruning(
            self.catalog,
            groups,
            packages,
            {"expansion_pwm"},
        )

        for package in (
            "service_audio",
            "service_synth",
            "service_expansion",
            "expansion_audio_pwm",
            "app_aplay",
            "app_player",
            "app_synth",
            "app_funcgen",
        ):
            self.assertTrue(pruned[package], package)

    def test_pcm5102_expansion_survives_without_builtin_audio(self):
        _, _, groups, packages = self.resolve("full")
        _, pruned = generate_flavor_config.apply_board_capability_pruning(
            self.catalog,
            groups,
            packages,
            {"expansion_i2s"},
        )

        for package in (
            "service_audio",
            "service_synth",
            "service_expansion",
            "expansion_pcm5102",
            "app_aplay",
            "app_player",
            "app_synth",
            "app_funcgen",
        ):
            self.assertTrue(pruned[package], package)

    def test_pcm1808_expansion_survives_without_builtin_audio(self):
        _, _, groups, packages = self.resolve("full")
        _, pruned = generate_flavor_config.apply_board_capability_pruning(
            self.catalog,
            groups,
            packages,
            {"expansion_i2s"},
        )

        for package in (
            "service_audio",
            "service_expansion",
            "expansion_pcm1808",
            "app_arecord",
            "app_recorder",
        ):
            self.assertTrue(pruned[package], package)

    def test_pcm1808_expansion_is_pruned_without_i2s_capability(self):
        _, _, groups, packages = self.resolve("full")
        _, pruned = generate_flavor_config.apply_board_capability_pruning(
            self.catalog,
            groups,
            packages,
            {"expansion_gpio"},
        )

        self.assertFalse(pruned["expansion_pcm1808"])

    def test_pcm5102_expansion_is_pruned_without_i2s_capability(self):
        _, _, groups, packages = self.resolve("full")
        _, pruned = generate_flavor_config.apply_board_capability_pruning(
            self.catalog,
            groups,
            packages,
            {"expansion_gpio"},
        )

        self.assertFalse(pruned["expansion_pcm5102"])

    def test_audio_backend_expansions_do_not_require_builtin_audio(self):
        _, _, groups, packages = self.resolve("full")

        _, s3 = generate_flavor_config.apply_board_capability_pruning(
            self.catalog,
            groups,
            packages,
            {"i2c", "expansion_i2s"},
        )
        self.assertTrue(s3["driver_audio_es8311_codecs"])

        _, classic = generate_flavor_config.apply_board_capability_pruning(
            self.catalog,
            groups,
            packages,
            {"expansion_gpio"},
        )
        self.assertTrue(classic["driver_audio_esp32_dac"])

    def test_rover_flavors_share_an_expansion_capable_baseline(self):
        rover_name, _, rover_groups, rover_packages = self.resolve("rover")
        python_name, _, python_groups, python_packages = self.resolve("rover-python")
        lua_name, _, lua_groups, lua_packages = self.resolve("rover-lua")

        self.assertEqual(rover_name, "rover")
        self.assertEqual(python_name, "rover-python")
        self.assertEqual(lua_name, "rover-lua")
        for groups in (rover_groups, python_groups, lua_groups):
            self.assertTrue(groups["system"])
            self.assertTrue(groups["expansions"])
            self.assertFalse(groups["maintenance_apps"])
            self.assertFalse(groups["maintenance_jobs"])
            self.assertFalse(groups["hardware_jobs"])
            self.assertFalse(groups["audio"])
            self.assertFalse(groups["agent"])
            self.assertTrue(groups["net"])
            self.assertTrue(groups["media"])
            self.assertTrue(groups["utils"])
        self.assertTrue(rover_groups["writing"])
        # The effective group remains visible because app_files is an explicit
        # writing-group trigger; the other writing packages stay disabled.
        self.assertTrue(python_groups["writing"])
        self.assertTrue(lua_groups["writing"])
        for packages in (rover_packages, python_packages, lua_packages):
            self.assertTrue(packages["service_expansion"])
            self.assertTrue(packages["app_files"])
            self.assertFalse(packages["service_ota"])
            self.assertFalse(packages["service_docs"])
            self.assertTrue(packages["job_log"])
            self.assertTrue(packages["job_bridge"])
            self.assertFalse(packages["job_batmon"])
            self.assertFalse(packages["job_daq"])
            self.assertFalse(packages["job_sump"])
            self.assertFalse(packages["app_agent"])
            self.assertFalse(packages["app_logic"])
            for audio_app in (
                "app_aplay",
                "app_arecord",
                "app_recorder",
                "app_player",
                "app_synth",
                "app_funcgen",
            ):
                self.assertFalse(packages[audio_app], audio_app)

        self.assertFalse(rover_groups["games"])
        self.assertFalse(rover_packages["app_invaders"])
        self.assertFalse(rover_packages["app_gameboy"])
        self.assertFalse(rover_packages["app_python"])
        self.assertFalse(rover_packages["app_lua"])
        self.assertFalse(python_groups["games"])
        self.assertFalse(python_packages["app_invaders"])
        self.assertTrue(python_groups["python"])
        self.assertTrue(python_packages["app_python"])
        self.assertFalse(python_packages["app_lua"])
        self.assertFalse(lua_groups["games"])
        self.assertFalse(lua_packages["app_invaders"])
        self.assertTrue(lua_groups["lua"])
        self.assertTrue(lua_packages["app_lua"])
        self.assertFalse(lua_packages["app_python"])

        python_difference = {
            package
            for package in rover_packages
            if rover_packages[package] != python_packages[package]
        }
        lua_difference = {
            package
            for package in rover_packages
            if rover_packages[package] != lua_packages[package]
        }
        self.assertEqual(
            python_difference,
            {
                "service_playground",
                "service_script_net",
                "service_script_runner",
                "app_python",
                "app_playground",
                "app_reader",
                "app_writer",
                "app_notes",
            },
        )
        self.assertEqual(
            lua_difference,
            {
                "service_playground",
                "service_script_net",
                "service_script_runner",
                "app_lua",
                "app_playground",
                "app_reader",
                "app_writer",
                "app_notes",
            },
        )

    def test_rover_synth_is_a_focused_ble_audio_expansion_flavor(self):
        name, _, groups, packages = self.resolve("rover-synth")

        self.assertEqual(name, "rover-synth")
        for group in (
            "system",
            "expansions",
            "maintenance_apps",
            "maintenance_jobs",
            "hardware_jobs",
            "net",
            "agent",
            "media",
            "games",
            "python",
            "lua",
            "utils",
        ):
            self.assertFalse(groups[group], group)

        for package in (
            "system_shell",
            "service_ble",
            "service_sd",
            "service_audio",
            "service_synth",
            "service_controls",
            "job_controls",
            "service_expansion",
            "expansion_audio_pwm",
            "app_synth",
            "job_midi",
            "job_log",
        ):
            self.assertTrue(packages[package], package)

        for package in (
            "service_wifi",
            "service_radio",
            "service_meshcore",
            "app_funcgen",
            "app_invaders",
            "app_view",
        ):
            self.assertFalse(packages[package], package)

    def test_existing_flavors_preserve_hardware_job_selection(self):
        for flavor in ("core", "full", "netrunner"):
            with self.subTest(flavor=flavor):
                _, _, groups, packages = self.resolve(flavor)
                self.assertTrue(groups["hardware_jobs"])
                self.assertTrue(packages["job_bridge"])
                self.assertTrue(packages["job_controls"])
                self.assertTrue(packages["job_daq"])
                self.assertTrue(packages["job_sump"])

        _, _, full_groups, full_packages = self.resolve("full")
        self.assertTrue(full_groups["writing"])
        for package in ("app_reader", "app_writer", "app_files", "app_notes"):
            self.assertTrue(full_packages[package], package)

    def test_gameboy_requires_a_streaming_display(self):
        _, _, groups, packages = self.resolve("full")
        _, pruned = generate_flavor_config.apply_board_capability_pruning(
            self.catalog,
            groups,
            packages,
            {"gfx", "psram", "sd"},
        )
        self.assertTrue(pruned["app_invaders"])
        self.assertFalse(pruned["app_gameboy"])

        _, capable = generate_flavor_config.apply_board_capability_pruning(
            self.catalog,
            groups,
            packages,
            {"gfx", "psram", "sd", "streaming_display"},
        )
        self.assertTrue(capable["app_gameboy"])

    def test_rover_gameboy_is_focused_and_omits_invaders(self):
        name, _, groups, packages = self.resolve("rover-gameboy")

        self.assertEqual(name, "rover-gameboy")
        self.assertTrue(groups["games"])
        self.assertTrue(packages["app_gameboy"])
        self.assertFalse(packages["app_invaders"])
        self.assertFalse(groups["media"])
        self.assertFalse(groups["audio"])
        self.assertTrue(packages["service_wifi"])
        self.assertTrue(packages["app_ssh"])
        self.assertTrue(packages["app_scp"])


if __name__ == "__main__":
    unittest.main()
