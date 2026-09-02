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

    def test_games_are_only_in_full(self):
        built_in_flavors = ("core", "full", "netrunner", "rover", "writerdeck")
        for flavor in built_in_flavors:
            _, _, groups, packages = self.resolve(flavor)
            expected = flavor == "full"
            self.assertEqual(groups["gameboy"], expected, flavor)
            self.assertEqual(packages["app_invaders"], expected,
                             flavor)
            self.assertEqual(packages["app_gameboy"], expected,
                             flavor)

    def test_update_layout_adds_ota_without_exposing_it_in_flavor(self):
        _, _, _, packages = self.resolve("core")
        single = generate_flavor_config.apply_update_layout(
            self.catalog, packages, "single"
        )
        ota = generate_flavor_config.apply_update_layout(
            self.catalog, packages, "ota"
        )
        self.assertFalse(single["service_ota"])
        self.assertTrue(ota["service_ota"])
        self.assertTrue(ota["service_http_client"])
        with self.assertRaisesRegex(ValueError, "unknown update layout"):
            generate_flavor_config.apply_update_layout(
                self.catalog, packages, "unknown"
            )

    def test_sketch_is_media_without_pointer_or_psram_gates(self):
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["sketch"])
        self.assertTrue(packages["app_sketch"])

        pruned_groups, pruned_packages = (
            generate_flavor_config.apply_board_capability_pruning(
                self.catalog,
                groups,
                packages,
                {"gfx"},
            )
        )
        self.assertTrue(pruned_groups["sketch"])
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
            self.catalog.group_defs["ssh"].members,
            ("app_ssh", "app_scp"),
        )
        self.assertEqual(
            self.catalog.group_defs["ftp"].members,
            ("app_ftp", "job_ftpd"),
        )
        self.assertEqual(
            self.catalog.group_defs["meshcore"].members,
            ("job_meshcore",),
        )
        self.assertEqual(
            self.catalog.group_defs["gameboy"].members,
            ("app_gameboy",),
        )
        self.assertEqual(
            self.catalog.group_defs["pcm1808"].members,
            ("expansion_pcm1808",),
        )
        self.assertEqual(self.catalog.group_defs["usb_hid"].members, ("service_hid",))
        self.assertTrue(self.catalog.group_defs["usb_hid"].hidden)
        self.assertTrue(self.catalog.group_defs["bootstrap"].hidden)
        self.assertEqual(self.catalog.group_defs["ssh"].category, "Networking")
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
        self.assertEqual(
            self.catalog.group_defs["wireguard"].members,
            ("service_wireguard",),
        )
        self.assertEqual(self.catalog.group_defs["osc"].members, ("job_osc",))
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
        self.assertTrue(groups["writer"])
        self.assertTrue(groups["logging"])
        self.assertFalse(groups["bridge"])
        self.assertFalse(groups["clock"])
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

    def test_standard_flavors_do_not_select_dormant_hid(self):
        for flavor in ("core", "full", "netrunner", "rover", "writerdeck"):
            _, _, groups, packages = self.resolve(flavor)
            self.assertFalse(groups["usb_hid"], flavor)
            self.assertFalse(packages["service_hid"], flavor)

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

    def test_rover_is_an_expansion_capable_baseline(self):
        rover_name, _, rover_groups, rover_packages = self.resolve("rover")

        self.assertEqual(rover_name, "rover")
        self.assertTrue(rover_groups["hardware_shell"])
        self.assertTrue(rover_groups["rfm69"])
        self.assertTrue(rover_groups["meshcore"])
        self.assertFalse(rover_groups["ota"])
        self.assertTrue(rover_groups["logging"])
        self.assertTrue(rover_groups["bridge"])
        self.assertFalse(rover_groups["audio_commands"])
        self.assertFalse(rover_groups["agent"])
        self.assertTrue(rover_groups["ssh"])
        self.assertTrue(rover_groups["image_viewer"])
        self.assertTrue(rover_groups["clock"])
        self.assertTrue(rover_groups["writer"])
        self.assertTrue(rover_packages["service_expansion"])
        self.assertTrue(rover_packages["app_files"])
        self.assertFalse(rover_packages["service_ota"])
        self.assertFalse(rover_packages["service_docs"])
        self.assertTrue(rover_packages["job_log"])
        self.assertTrue(rover_packages["job_bridge"])
        self.assertFalse(rover_packages["job_batmon"])
        self.assertFalse(rover_packages["job_daq"])
        self.assertFalse(rover_packages["job_sump"])
        self.assertFalse(rover_packages["app_agent"])
        self.assertFalse(rover_packages["app_logic"])
        self.assertFalse(rover_groups["gameboy"])
        self.assertFalse(rover_packages["app_invaders"])
        self.assertFalse(rover_packages["app_gameboy"])
        self.assertFalse(rover_packages["app_python"])
        self.assertFalse(rover_packages["app_lua"])

    def test_existing_flavors_preserve_hardware_job_selection(self):
        for flavor in ("core", "full", "netrunner"):
            with self.subTest(flavor=flavor):
                _, _, groups, packages = self.resolve(flavor)
                self.assertTrue(groups["bridge"])
                self.assertTrue(packages["job_bridge"])
                self.assertTrue(packages["job_controls"])
                self.assertTrue(packages["job_daq"])
                self.assertTrue(packages["job_sump"])

        _, _, full_groups, full_packages = self.resolve("full")
        self.assertTrue(full_groups["writer"])
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

if __name__ == "__main__":
    unittest.main()
