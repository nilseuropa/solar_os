import importlib.util
from pathlib import Path
import sys
import tomllib
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

    def test_public_flavor_name_can_override_package_configuration_name(self):
        self.assertEqual(
            generate_flavor_config.public_flavor_name("full", "beta"),
            "beta",
        )
        self.assertEqual(
            generate_flavor_config.public_flavor_name("full", ""),
            "full",
        )
        with self.assertRaisesRegex(ValueError, "invalid flavor name"):
            generate_flavor_config.public_flavor_name("full", "beta flavor")

    def test_radio_scanner_requires_graphics_and_radio_service(self):
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["radio_scanner"])
        for name in ("app_rfscan", "service_radio_scan", "service_radio", "service_audio"):
            self.assertTrue(packages[name], name)
        _, capable = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, {"gfx", "expansion_spi"})
        self.assertTrue(capable["app_rfscan"])
        _, headless = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, {"expansion_spi"})
        self.assertFalse(headless["app_rfscan"])
        for flavor in ("classic", "writerdeck"):
            _, _, _, optional = self.resolve(flavor)
            self.assertFalse(optional["app_rfscan"])

    def test_games_are_only_in_full(self):
        built_in_flavors = ("full", "classic", "writerdeck")
        for flavor in built_in_flavors:
            _, _, groups, packages = self.resolve(flavor)
            expected = flavor == "full"
            self.assertEqual(packages["app_invaders"], expected,
                             flavor)
            self.assertNotIn("gameboy", groups, flavor)
            self.assertNotIn("app_gameboy", packages, flavor)

    def test_tcp_byte_streams_are_optional_and_enabled_in_full(self):
        for flavor in ("full", "classic", "writerdeck"):
            _, _, groups, packages = self.resolve(flavor)
            self.assertEqual(groups["tcp_streams"], flavor == "full")
            self.assertEqual(packages["service_tcp_stream"], flavor == "full")

    def test_classic_uses_compact_baseline_with_dpad_support(self):
        _, _, compact_groups, compact_packages = self.resolve("writerdeck")
        name, _, groups, packages = self.resolve("classic")
        self.assertEqual(name, "classic")
        self.assertEqual({key for key in compact_groups if compact_groups[key] != groups[key]},
                         {"adc_dpad", "synth", "sc2"})
        self.assertEqual({key for key in compact_packages if compact_packages[key] != packages[key]},
                         {"service_adc_dpad", "app_synth", "service_synth", "service_midi",
                          "service_sc2", "service_sc2_provider", "job_sc2_sync"})
        self.assertTrue(packages["service_adc_dpad"])
        self.assertFalse(packages["app_lua"])
        self.assertTrue(packages["app_python"])
        self.assertTrue(packages["app_playground"])
        requires = generate_flavor_config.collect_requires(self.catalog, groups, packages)
        self.assertNotIn("lua", requires)
        self.assertIn("micropython_embed", requires)

    def test_servo_is_optional_and_native_backend_requires_pwm_gpio(self):
        _, _, core_groups, core_packages = self.resolve("writerdeck")
        self.assertFalse(core_groups["servo"])
        self.assertFalse(core_packages["service_servo"])
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["servo"])
        self.assertTrue(packages["service_servo"])
        self.assertTrue(packages["service_resources"])
        for capabilities, enabled in (({"pwm", "gpio"}, True),
                                      ({"gpio"}, False), ({"pwm"}, False)):
            with self.subTest(capabilities=capabilities):
                _, pruned = generate_flavor_config.apply_board_capability_pruning(
                    self.catalog, groups, packages, capabilities
                )
                self.assertTrue(pruned["service_servo"])
                self.assertTrue(pruned["service_pwm"])
                self.assertEqual(pruned["driver_pwm_native"], enabled)

    def test_drv8830_is_optional_and_requires_expansion_i2c(self):
        for flavor in ("classic", "writerdeck"):
            _, _, groups, packages = self.resolve(flavor)
            self.assertFalse(groups["drv8830"])
            self.assertFalse(packages["drv8830"])
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["drv8830"])
        for target in ("esp32", "esp32s3"):
            selected = generate_flavor_config.apply_target_pruning(self.catalog, packages, target)
            _, supported = generate_flavor_config.apply_board_capability_pruning(
                self.catalog, groups, selected, {"i2c", "expansion_i2c"})
            self.assertTrue(supported["drv8830"])
            self.assertTrue(supported["service_motor"])
            self.assertTrue(supported["service_i2c"])
            _, unsupported = generate_flavor_config.apply_board_capability_pruning(
                self.catalog, groups, selected, {"i2c"})
            self.assertFalse(unsupported["drv8830"])
            self.assertTrue(unsupported["service_motor"])

    def test_encoder_driver_is_optional_and_requires_gpio(self):
        for flavor in ("classic", "writerdeck"):
            _, _, groups, packages = self.resolve(flavor)
            self.assertFalse(groups["pcnt_encoder"])
            self.assertFalse(packages["pcnt_encoder"])
        _, _, groups, packages = self.resolve("full")
        for target in ("esp32", "esp32s3"):
            selected = generate_flavor_config.apply_target_pruning(self.catalog, packages, target)
            _, supported = generate_flavor_config.apply_board_capability_pruning(
                self.catalog, groups, selected, {"gpio"})
            self.assertTrue(supported["pcnt_encoder"])
            self.assertTrue(supported["service_encoder"])
            _, unsupported = generate_flavor_config.apply_board_capability_pruning(
                self.catalog, groups, selected, set())
            self.assertFalse(unsupported["pcnt_encoder"])
            self.assertTrue(unsupported["service_encoder"])

    def test_encoder_service_does_not_pull_a_concrete_driver(self):
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "encoder.toml"
            path.write_text('[flavor]\nname = "encoder-only"\n[groups]\nencoder = true\n')
            _, _, groups, packages = generate_flavor_config.load_flavor(path, self.catalog)
        self.assertTrue(packages["service_encoder"])
        self.assertFalse(packages["pcnt_encoder"])
        self.assertFalse(packages["rotary_encoder"])
        _, pruned = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, set())
        self.assertTrue(pruned["service_encoder"])

    def test_motor_service_does_not_pull_a_concrete_driver(self):
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "motor.toml"
            path.write_text('[flavor]\nname = "motor-only"\n[groups]\nmotor = true\n')
            _, _, groups, packages = generate_flavor_config.load_flavor(path, self.catalog)
        self.assertTrue(packages["service_motor"])
        self.assertFalse(packages["drv8830"])
        self.assertFalse(packages["service_i2c"])
        _, pruned = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, set())
        self.assertTrue(pruned["service_motor"])

    def test_pca9685_is_optional_and_uses_i2c_without_native_pwm(self):
        _, _, core_groups, core_packages = self.resolve("writerdeck")
        self.assertFalse(core_groups["pca9685"])
        self.assertFalse(core_packages["pca9685"])
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["pca9685"])
        for target in ("esp32", "esp32s3"):
            target_packages = generate_flavor_config.apply_target_pruning(self.catalog, packages, target)
            _, supported = generate_flavor_config.apply_board_capability_pruning(
                self.catalog, groups, target_packages, {"i2c", "expansion_i2c"})
            self.assertTrue(supported["pca9685"])
            self.assertTrue(supported["service_pwm"])
            self.assertFalse(supported["driver_pwm_native"])
            _, unsupported = generate_flavor_config.apply_board_capability_pruning(
                self.catalog, groups, target_packages, {"i2c"})
            self.assertFalse(unsupported["pca9685"])

    def test_backlight_drivers_link_pwm_without_native_pwm_capability(self):
        board = tomllib.loads(
            (REPOSITORY / "boards" / "manifests" /
             "waveshare_esp32_s3_epaper_3_97.toml").read_text()
        )
        _, _, groups, packages = self.resolve("full")
        cases = (
            (board["target"]["mcu"], set(board["build"]["capabilities"]),
             ("expansion_lilygo_pager_keyboard", "driver_display_ili9341",
              "driver_display_st7796", "driver_display_st7789")),
            ("esp32", {"gfx", "spi", "expansion_gpio"},
             ("driver_display_ili9341", "driver_display_st7796")),
        )
        for target, capabilities, consumers in cases:
            with self.subTest(target=target, capabilities=capabilities):
                target_packages = generate_flavor_config.apply_target_pruning(
                    self.catalog, packages, target
                )
                pruned_groups, pruned = generate_flavor_config.apply_board_capability_pruning(
                    self.catalog, groups, target_packages, capabilities
                )
                self.assertFalse(pruned["driver_pwm_native"])
                for consumer in consumers:
                    self.assertTrue(pruned[consumer], consumer)
                    selected = generate_flavor_config.enable_required_packages(
                        self.catalog, dict.fromkeys(packages, False), {consumer}
                    )
                    _, selected = generate_flavor_config.apply_board_capability_pruning(
                        self.catalog, groups, selected, capabilities
                    )
                    self.assertIn("drivers/pwm_port.c", generate_flavor_config.collect_sources(
                        self.catalog, groups, selected
                    ), consumer)
                sources = generate_flavor_config.collect_sources(
                    self.catalog, pruned_groups, pruned
                )
                self.assertIn("drivers/pwm_port.c", sources)
                self.assertIn("drivers/gpio_port.c", sources)
                self.assertNotIn("services/solar_os_pwm_native.c", sources)
                self.assertIn("esp_driver_ledc", generate_flavor_config.collect_requires(
                    self.catalog, pruned_groups, pruned
                ))

    def test_odroid_runtime_i2c_survives_board_and_target_pruning(self):
        board = tomllib.loads(
            (REPOSITORY / "boards" / "manifests" / "odroid_go.toml").read_text()
        )
        capabilities = set(board["build"]["capabilities"])
        for flavor in ("writerdeck", "full"):
            with self.subTest(flavor=flavor):
                _, _, groups, packages = self.resolve(flavor)
                _, pruned = generate_flavor_config.apply_board_capability_pruning(
                    self.catalog, groups, packages, capabilities
                )
                pruned = generate_flavor_config.apply_target_pruning(
                    self.catalog, pruned, board["target"]["mcu"]
                )
                for package in ("service_resources", "service_i2c",
                                "service_expansion"):
                    self.assertTrue(pruned[package], package)
                if flavor == "full":
                    self.assertTrue(pruned["expansion_cardkb"])
                    self.assertTrue(pruned["expansion_tab5_keyboard"])
                    self.assertTrue(pruned["expansion_inputronic_keyboard"])

    def test_native_image_pipeline_does_not_require_espdl(self):
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["pipelines"])
        self.assertTrue(packages["service_pipeline"])
        classic = generate_flavor_config.apply_target_pruning(self.catalog, packages, "esp32")
        self.assertTrue(classic["service_pipeline"])
        self.assertTrue(classic["service_vision"])
        self.assertFalse(classic["service_inference"])
        self.assertNotIn("service_inference", self.catalog.package_defs["service_pipeline"].depends)

    def test_zoo_works_on_headless_s3_and_is_pruned_without_platform_requirements(self):
        _, _, groups, packages = self.resolve("full")
        _, headless = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, {"psram", "wifi"})
        self.assertTrue(headless["app_zoo"])
        self.assertTrue(headless["service_zoo"])
        self.assertTrue(headless["service_inference"])
        classic = generate_flavor_config.apply_target_pruning(self.catalog, packages, "esp32")
        self.assertFalse(classic["app_zoo"])
        self.assertFalse(classic["service_zoo"])
        for caps in ({"wifi"}, {"psram"}, set()):
            _, pruned = generate_flavor_config.apply_board_capability_pruning(self.catalog, groups, packages, caps)
            self.assertFalse(pruned["app_zoo"])

    def test_update_layout_adds_ota_without_exposing_it_in_flavor(self):
        _, _, _, packages = self.resolve("writerdeck")
        packages["service_ota"] = False
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

    def test_uc8279_expansion_profile_and_package(self):
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["uc8279"])
        self.assertTrue(packages["expansion_uc8279"])
        self.assertTrue(packages["driver_epd_ultrachip"])
        _, supported = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, {"gfx", "spi", "expansion_spi", "expansion_gpio"}
        )
        self.assertTrue(supported["expansion_uc8279"])
        _, unavailable = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, {"gfx", "spi", "expansion_spi"}
        )
        self.assertFalse(unavailable["expansion_uc8279"])

    def test_ssd1685_expansion_profile_and_package(self):
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["ssd1685"])
        self.assertTrue(packages["expansion_ssd1685"])
        for caps, enabled in (
            ({"gfx", "spi", "expansion_spi", "expansion_gpio"}, True),
            ({"gfx", "spi", "expansion_spi"}, False),
            ({"spi", "expansion_spi", "expansion_gpio"}, False),
        ):
            _, selected = generate_flavor_config.apply_board_capability_pruning(
                self.catalog, groups, packages, caps)
            self.assertEqual(selected["expansion_ssd1685"], enabled)
        for flavor in ("classic", "writerdeck"):
            _, _, _, optional = self.resolve(flavor)
            self.assertFalse(optional["expansion_ssd1685"])

    def test_uc8179_expansion_profile_and_package(self):
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["uc8179"])
        self.assertTrue(packages["expansion_uc8179"])
        self.assertTrue(packages["driver_epd_ultrachip"])
        _, supported = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, {"gfx", "spi", "expansion_spi", "expansion_gpio"}
        )
        self.assertTrue(supported["expansion_uc8179"])
        _, unavailable = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, {"gfx", "spi", "expansion_spi"}
        )
        self.assertFalse(unavailable["expansion_uc8179"])

    def test_cw2017_expansion_only_requires_i2c_hardware(self):
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["cw2017"])
        self.assertTrue(packages["cw2017"])
        _, supported = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, {"i2c"}
        )
        self.assertTrue(supported["cw2017"])
        _, unavailable = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, set()
        )
        self.assertFalse(unavailable["cw2017"])

    def test_pcf8563_expansion_only_requires_i2c_hardware(self):
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["pcf8563"])
        self.assertTrue(packages["driver_pcf8563"])
        _, supported = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, {"i2c"}
        )
        for target in ("esp32", "esp32s3"):
            pruned = generate_flavor_config.apply_target_pruning(self.catalog, supported, target)
            self.assertTrue(pruned["driver_pcf8563"], target)
        _, unavailable = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, set()
        )
        self.assertFalse(unavailable["driver_pcf8563"])

    def test_ds3231_expansion_only_requires_i2c_hardware(self):
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["ds3231"])
        self.assertTrue(packages["driver_ds3231"])
        _, _, core_groups, core_packages = self.resolve("writerdeck")
        self.assertFalse(core_groups["ds3231"])
        self.assertFalse(core_packages["driver_ds3231"])
        _, supported = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, {"i2c"}
        )
        for target in ("esp32", "esp32s3"):
            pruned = generate_flavor_config.apply_target_pruning(self.catalog, supported, target)
            self.assertTrue(pruned["driver_ds3231"], target)
        _, unavailable = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, set()
        )
        self.assertFalse(unavailable["driver_ds3231"])

    def test_gesture_listener_job_is_part_of_the_core_runtime(self):
        for flavor in ("full", "writerdeck"):
            _, _, _, packages = self.resolve(flavor)
            self.assertTrue(packages["job_gesture_listener"], flavor)

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

    def test_vplay_is_separate_from_the_image_viewer(self):
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["vplay"])
        self.assertTrue(packages["app_vplay"])
        self.assertNotIn("mplayer", groups)
        self.assertNotIn("app_mplayer", packages)
        self.assertIn("service_media_widgets", self.catalog.package_defs["app_vplay"].depends)
        self.assertTrue(packages["service_mpeg"])
        view = self.catalog.package_defs["app_view"]
        self.assertNotIn("service_mpeg", view.depends)
        self.assertNotIn("service_audio", view.depends)
        self.assertEqual(view.sources, ("apps/solar_os_view.c",))
        for caps, expected in (({"psram", "gfx"}, True), ({"psram"}, False), ({"gfx"}, False)):
            _, pruned = generate_flavor_config.apply_board_capability_pruning(
                self.catalog, groups, packages, caps)
            self.assertEqual(pruned["app_vplay"], expected)

    def test_graffiti_is_available_for_runtime_attached_pointers(self):
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["handwriting_input"])
        self.assertTrue(packages["job_graffiti"])

        pruned_groups, pruned_packages = (
            generate_flavor_config.apply_board_capability_pruning(
                self.catalog,
                groups,
                packages,
                {"psram"},
            )
        )
        self.assertTrue(pruned_groups["handwriting_input"])
        self.assertTrue(pruned_packages["job_graffiti"])

    def test_launcher_is_available_on_graphics_builds(self):
        for flavor in ("full", "writerdeck"):
            groups, packages = self.resolve(flavor)[2:]
            self.assertTrue(groups["launcher"], flavor)
            self.assertTrue(packages["app_launcher"], flavor)

        _, _, groups, packages = self.resolve("writerdeck")
        _, graphics = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, {"gfx"}
        )
        _, headless = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, set()
        )
        self.assertTrue(graphics["app_launcher"])
        self.assertFalse(headless["app_launcher"])

    def test_board_required_package_enables_dependencies(self):
        _, _, _, packages = self.resolve("writerdeck")
        enabled = generate_flavor_config.enable_required_packages(
            self.catalog,
            packages,
            {"job_ps2_keyboard"},
        )

        self.assertTrue(enabled["job_ps2_keyboard"])
        self.assertTrue(enabled["service_ps2"])
        self.assertTrue(enabled["service_resources"])

    def test_solar_writer_rtc_survives_writerdeck_pruning(self):
        from solaros_board_manifest import load_board_manifest, load_driver_catalog, required_packages
        manifest_dir = REPOSITORY / "boards/manifests"
        board = load_board_manifest(manifest_dir / "solar_writer.toml", manifest_dir)
        drivers = load_driver_catalog(REPOSITORY / "boards/expansion_drivers.toml")
        _, _, groups, packages = self.resolve("writerdeck")
        self.assertFalse(packages["driver_ds3231"])
        selected = generate_flavor_config.enable_required_packages(
            self.catalog, packages, set(required_packages(board, drivers))
        )
        selected = generate_flavor_config.apply_target_pruning(
            self.catalog, selected, board["target"]["mcu"]
        )
        groups, selected = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, selected, set(board["build"]["capabilities"])
        )
        for package in ("driver_ds3231", "service_rtc", "service_i2c", "service_expansion"):
            self.assertTrue(selected[package], package)
        sources = generate_flavor_config.collect_sources(self.catalog, groups, selected)
        self.assertIn("drivers/rtc_ds3231.c", sources)
        self.assertIn("services/solar_os_ds3231_driver.c", sources)

    def test_cl32_epd_keeps_shared_pulse_sources_in_writerdeck(self):
        from solaros_board_manifest import load_board_manifest, load_driver_catalog, required_packages
        manifest_dir = REPOSITORY / "boards/manifests"
        board = load_board_manifest(manifest_dir / "cl32_epd.toml", manifest_dir)
        drivers = load_driver_catalog(REPOSITORY / "boards/expansion_drivers.toml")
        _, _, groups, packages = self.resolve("writerdeck")
        selected = generate_flavor_config.enable_required_packages(
            self.catalog, packages, set(required_packages(board, drivers)))
        groups, selected = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, selected, set(board["build"]["capabilities"]))
        sources = generate_flavor_config.collect_sources(self.catalog, groups, selected)
        for source in ("epd_ssd1685", "epd_ssd1685_waveform", "epd_pixel_budget", "epd_scan_worker"):
            self.assertIn(f"drivers/{source}.c", sources)
        self.assertNotIn("drivers/epd_ssd1683.c", sources)

    def test_unknown_board_required_package_is_rejected(self):
        _, _, _, packages = self.resolve("writerdeck")
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

    def test_camera_expansion_is_available_without_fitted_camera_on_s3(self):
        _, _, groups, packages = self.resolve("full")
        _, pruned = generate_flavor_config.apply_board_capability_pruning(
            self.catalog, groups, packages, {"psram", "expansion_gpio", "wifi"})
        s3 = generate_flavor_config.apply_target_pruning(self.catalog, pruned, "esp32s3")
        classic = generate_flavor_config.apply_target_pruning(self.catalog, pruned, "esp32")
        for name in ("driver_camera_esp32", "service_camera", "job_cam_webd"):
            self.assertTrue(s3[name], name)
            self.assertFalse(classic[name], name)
        self.assertTrue(s3["job_rtspd"])
        self.assertTrue(classic["job_rtspd"])

    def test_inputronic_keyboard_is_reusable_on_both_targets(self):
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["inputronic_keyboard"])
        self.assertEqual(self.catalog.group_defs["inputronic_keyboard"].category,
                         "Expansion hardware")
        for target in ("esp32", "esp32s3"):
            with self.subTest(target=target):
                pruned = generate_flavor_config.apply_target_pruning(
                    self.catalog, packages, target)
                self.assertTrue(pruned["expansion_inputronic_keyboard"])
                self.assertIn("solar_os_inputronic_keyboard_expansion_driver",
                              generate_flavor_config.collect_expansion_drivers(
                                  self.catalog, pruned))

    def test_tab5_keyboard_requires_expansion_i2c_on_both_targets(self):
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["tab5_keyboard"])
        for target in ("esp32", "esp32s3"):
            for capabilities, expected in (({"i2c", "expansion_i2c"}, True), ({"i2c"}, False)):
                with self.subTest(target=target, capabilities=capabilities):
                    _, pruned = generate_flavor_config.apply_board_capability_pruning(
                        self.catalog, groups, packages, capabilities)
                    pruned = generate_flavor_config.apply_target_pruning(self.catalog, pruned, target)
                    self.assertEqual(pruned["expansion_tab5_keyboard"], expected)
                    drivers = generate_flavor_config.collect_expansion_drivers(self.catalog, pruned)
                    self.assertEqual("solar_os_tab5_keyboard_expansion_driver" in drivers, expected)
                    if expected:
                        self.assertTrue(pruned["service_input_keymap"])

    def test_tca8418_profiles_share_backend_without_generic_pwm_dependency(self):
        _, _, _, packages = self.resolve("full")
        backend = self.catalog.package_defs["service_tca8418"]
        self.assertNotIn("service_pwm", backend.depends)
        for package in ("tca8418", "expansion_inputronic_keyboard", "expansion_lilygo_pager_keyboard"):
            self.assertIn("service_tca8418", self.catalog.package_defs[package].depends)
            for target in ("esp32", "esp32s3"):
                pruned = generate_flavor_config.apply_target_pruning(self.catalog, packages, target)
                self.assertTrue(pruned[package])
                self.assertTrue(pruned["service_tca8418"])

    def test_input_keymap_service_has_no_expansion_hardware_requirement(self):
        _, _, groups, packages = self.resolve("full")
        self.assertTrue(groups["input_keymap"])
        self.assertIn("service_input_keymap", self.catalog.package_defs["service_tca8418"].depends)
        service = self.catalog.package_defs["service_input_keymap"]
        self.assertEqual(service.capabilities, ())
        self.assertEqual(service.depends, ("service_json",))
        for target in ("esp32", "esp32s3"):
            target_packages = generate_flavor_config.apply_target_pruning(self.catalog, packages, target)
            _, no_hardware = generate_flavor_config.apply_board_capability_pruning(
                self.catalog, groups, target_packages, set())
            self.assertTrue(no_hardware["service_input_keymap"])
            self.assertFalse(no_hardware["service_tca8418"])

    def test_full_exposes_reusable_t_lora_expansion_drivers(self):
        _, _, groups, packages = self.resolve("full")
        reusable = {
            "xl9555": "xl9555",
            "tca8418": "tca8418",
            "lilygo_pager_keyboard": "expansion_lilygo_pager_keyboard",
            "sx1262": "sx1262",
            "lr11xx": "lr11xx",
            "rotary_encoder": "rotary_encoder",
            "bq27220": "bq27220",
            "bq25896": "bq25896",
            "qmi8658": "qmi8658",
        }
        for group, package in reusable.items():
            with self.subTest(group=group):
                self.assertFalse(self.catalog.group_defs[group].hidden)
                self.assertEqual(
                    self.catalog.group_defs[group].category,
                    "Expansion hardware",
                )
                self.assertTrue(groups[group])
                self.assertTrue(packages[package])

        s3 = generate_flavor_config.apply_target_pruning(
            self.catalog,
            packages,
            "esp32s3",
        )
        drivers = generate_flavor_config.collect_expansion_drivers(
            self.catalog,
            s3,
        )
        for symbol in (
            "solar_os_xl9555_expansion_driver",
            "solar_os_tca8418_expansion_driver",
            "solar_os_lilygo_pager_keyboard_expansion_driver",
            "solar_os_sx1262_expansion_driver",
            "solar_os_lr11xx_expansion_driver",
            "solar_os_rotary_encoder_expansion_driver",
            "solar_os_bq27220_expansion_driver",
            "solar_os_bq25896_expansion_driver",
            "solar_os_qmi8658_expansion_driver",
        ):
            self.assertIn(symbol, drivers)

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
            "expansion_ssd1685",
            "expansion_uc8179",
            "expansion_uc8279",
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
            ("app_ssh", "app_scp", "app_sftp", "app_sftpsync"),
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
            self.catalog.group_defs["meshcore_ble"].members,
            ("job_meshcore_ble",),
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
            ("service_schedule", "service_streams"),
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
            ("service_expansion", "service_spi", "driver_epd_pixel_budget", "driver_epd_scan_worker"),
        )
        self.assertEqual(
            self.catalog.package_defs["expansion_ssd1683"].capabilities,
            ("gfx", "expansion_gpio"),
        )
        self.assertEqual(
            self.catalog.package_defs["expansion_ssd1677"].depends,
            ("axp2101", "service_expansion", "service_spi"),
        )
        self.assertEqual(
            self.catalog.package_defs["expansion_ssd1677"].capabilities,
            ("gfx",),
        )
        self.assertEqual(
            self.catalog.group_defs["axp2101"].members,
            ("axp2101",),
        )
        self.assertEqual(
            self.catalog.package_defs["axp2101"].depends,
            (
                "driver_axp2101", "service_battery", "service_charger",
                "service_expansion", "service_i2c",
            ),
        )
        self.assertEqual(
            self.catalog.package_defs["driver_axp2101"].sources,
            ("drivers/axp2101.c",),
        )
        self.assertEqual(
            self.catalog.group_defs["qmi8658"].members,
            ("qmi8658",),
        )
        self.assertEqual(
            self.catalog.package_defs["qmi8658"].depends,
            ("service_expansion", "service_i2c", "service_imu"),
        )
        self.assertEqual(
            self.catalog.package_defs["qmi8658"].sources,
            (
                "drivers/qmi8658.c", "services/solar_os_qmi8658.c",
                "services/solar_os_qmi8658_driver.c",
            ),
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
            ("service_network",),
        )
        self.assertEqual(
            self.catalog.package_defs["service_wireguard"].capabilities,
            (),
        )
        self.assertNotIn(
            "esp_wifi",
            self.catalog.package_defs["service_wireguard"].requires,
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

    def test_writerdeck_selects_writing_media_scripting_and_file_transfer(self):
        name, _, groups, packages = self.resolve("writerdeck")

        self.assertEqual(name, "writerdeck")
        for group in (
            "editor",
            "pager",
            "files",
            "ssh",
            "http_client",
            "ftp",
            "audio_commands",
            "reader",
            "writer",
            "notes",
            "image_viewer",
            "python",
            "playground",
            "speech",
            "player",
            "recorder",
            "synth",
            "audio_pwm",
            "pcm5102",
            "web_browser",
            "uart",
            "bridge",
            "daq",
            "wireguard",
            "espnow",
            "contacts",
            "inbox",
            "chat",
            "clock",
            "calculator",
            "plot",
            "sheet",
        ):
            self.assertTrue(groups[group], group)
        self.assertFalse(groups["rtsp"])
        self.assertFalse(packages["app_rtsp"])
        self.assertFalse(packages["job_rtspd"])
        for package in (
            "app_edit",
            "app_less",
            "app_reader",
            "app_writer",
            "app_files",
            "app_notes",
            "app_ssh",
            "app_scp",
            "app_sftp",
            "app_sftpsync",
            "app_ftp",
            "job_ftpd",
            "app_python",
            "app_playground",
            "app_aplay",
            "app_view",
            "app_player",
            "app_recorder",
            "app_synth",
            "service_synth",
            "service_speech",
            "job_speechd",
            "expansion_audio_pwm",
            "expansion_pcm5102",
            "job_bridge",
            "job_daq",
            "service_wireguard",
            "service_contacts",
            "service_inbox",
            "service_messaging",
            "service_uart",
            "job_espnow_link",
            "app_com",
            "app_web",
            "app_clock",
            "app_calc",
            "app_plot",
            "app_sheet",
        ):
            self.assertTrue(packages[package], package)
        self.assertTrue(packages["job_controls"])
        for group in (
            "device_flasher",
            "logic_analyzer",
            "sump",
            "mqtt",
            "slip",
            "ppp",
            "osc",
            "pocsag",
            "radio_link",
            "meshcore",
            "meshcore_ble",
            "rfm69",
            "rfm95",
            "sx1262",
            "lr11xx",
        ):
            self.assertFalse(groups[group], group)
        for package in (
            "job_sump",
            "service_mqtt",
            "job_slip",
            "job_pppd",
            "job_osc",
            "job_pocsag",
            "job_radio_link",
            "job_meshcore",
            "job_meshcore_ble",
            "app_lua",
            "app_logic",
        ):
            self.assertFalse(packages[package], package)

    def test_speech_survives_with_attachable_audio_output(self):
        _, _, groups, packages = self.resolve("full")
        for capabilities in ({"psram", "expansion_i2s"},
                             {"psram", "expansion_pwm"}):
            _, pruned = generate_flavor_config.apply_board_capability_pruning(
                self.catalog,
                groups,
                packages,
                capabilities,
            )
            self.assertTrue(pruned["service_speech"], capabilities)
            self.assertTrue(pruned["job_speechd"], capabilities)

    def test_script_runtimes_do_not_force_optional_protocol_services(self):
        for runtime in ("app_python", "app_lua"):
            dependencies = self.catalog.package_defs[runtime].depends
            self.assertIn("service_script_net", dependencies)
            self.assertIn("service_script_runner", dependencies)
            self.assertNotIn("service_ftp", dependencies)
            self.assertNotIn("service_messaging", dependencies)

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
        self.assertTrue(pruned["service_wireguard"])
        self.assertFalse(pruned["job_espnow_link"])

    def test_standard_flavors_do_not_select_dormant_hid(self):
        for flavor in ("full", "writerdeck"):
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

    def test_compact_retains_installation_and_scripting_on_supported_targets(self):
        from solaros_board_manifest import load_board_manifest, load_driver_catalog, required_packages
        manifest_dir = REPOSITORY / "boards/manifests"
        drivers = load_driver_catalog(REPOSITORY / "boards/expansion_drivers.toml")
        cases = [("writerdeck", board) for board in
                 ("elecrow_crowpanel_esp32_s3_5_79_epaper",
                  "freenove_esp32_wrover_v3", "ttgo_vga32_v14")]
        cases += [("classic", board) for board in
                  ("odroid_go", "esp32_devkitc_v4_wrover",
                   "freenove_esp32_wrover_v3", "ttgo_vga32_v14")]
        for flavor, board_id in cases:
            with self.subTest(flavor=flavor, board=board_id):
                _, _, groups, packages = self.resolve(flavor)
                board = tomllib.loads((REPOSITORY / "boards/manifests" /
                                       f"{board_id}.toml").read_text())
                selected = generate_flavor_config.enable_required_packages(
                    self.catalog, packages, set(required_packages(
                        load_board_manifest(manifest_dir / f"{board_id}.toml", manifest_dir), drivers)))
                selected = generate_flavor_config.apply_target_pruning(
                    self.catalog, selected, board["target"]["mcu"])
                _, selected = generate_flavor_config.apply_board_capability_pruning(
                    self.catalog, groups, selected, set(board["build"]["capabilities"]))
                self.assertTrue(selected["app_python"])
                self.assertTrue(selected["app_playground"])
                self.assertEqual(selected["service_native_modules"],
                                 board["target"]["mcu"] == "esp32s3")
                if board_id == "ttgo_vga32_v14":
                    self.assertTrue(selected["expansion_ps2_keyboard"])
                    self.assertTrue(selected["service_ps2"])
                if board_id == "odroid_go":
                    for package in ("service_buttons", "service_adc_dpad",
                                    "driver_display_ili9341", "driver_audio_esp32_dac",
                                    "expansion_sdspi"):
                        self.assertTrue(selected[package], package)

    def test_public_profiles_and_board_defaults(self):
        self.assertEqual({path.stem for path in (REPOSITORY / "flavors").glob("*.toml")},
                         {"full", "classic", "writerdeck"})
        import configparser
        config = configparser.ConfigParser()
        config.read(REPOSITORY / "platformio.ini")
        classic_boards = {
            path.stem for path in (REPOSITORY / "boards/manifests").glob("*.toml")
            if tomllib.loads(path.read_text()).get("target", {}).get("mcu") == "esp32"
        }
        self.assertEqual(classic_boards, {"odroid_go", "esp32_devkitc_v4_wrover",
                                         "freenove_esp32_wrover_v3", "ttgo_vga32_v14"})
        for board in classic_boards:
            with self.subTest(board=board):
                self.assertEqual(config[f"env:{board}"]["custom_solaros_default_flavor"],
                                 "classic")
        for board in ("elecrow_crowpanel_esp32_s3_5_79_epaper",
                      "elecrow_crowpanel_esp32_s3_4_2_epaper"):
            self.assertEqual(config[f"env:{board}"]["custom_solaros_default_flavor"], "writerdeck")

    def test_existing_flavors_preserve_hardware_job_selection(self):
        for flavor in ("writerdeck", "full"):
            with self.subTest(flavor=flavor):
                _, _, groups, packages = self.resolve(flavor)
                self.assertTrue(groups["bridge"])
                self.assertTrue(packages["job_bridge"])
                self.assertTrue(packages["job_controls"])
                self.assertTrue(packages["job_daq"])
                self.assertEqual(packages["job_sump"], flavor == "full")

        _, _, full_groups, full_packages = self.resolve("full")
        self.assertTrue(full_groups["writer"])
        for package in ("app_reader", "app_writer", "app_files", "app_notes"):
            self.assertTrue(full_packages[package], package)

    def test_gameboy_is_external_to_all_firmware_packages(self):
        _, _, groups, packages = self.resolve("full")
        _, pruned = generate_flavor_config.apply_board_capability_pruning(
            self.catalog,
            groups,
            packages,
            {"gfx", "psram", "sd"},
        )
        self.assertTrue(pruned["app_invaders"])
        self.assertNotIn("app_gameboy", pruned)

        _, capable = generate_flavor_config.apply_board_capability_pruning(
            self.catalog,
            groups,
            packages,
            {"gfx", "psram", "sd", "streaming_display"},
        )
        self.assertNotIn("app_gameboy", capable)
        sources = generate_flavor_config.collect_sources(self.catalog, groups, capable)
        self.assertFalse(any("gameboy" in source for source in sources))

if __name__ == "__main__":
    unittest.main()
