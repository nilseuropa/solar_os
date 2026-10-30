from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
GUARD = ROOT / "scripts/classic_psram_sdkconfig.cmake"
DEFAULTS = ROOT / "sdkconfig.defaults.esp32_psram"
CLASSIC = 'CONFIG_IDF_TARGET="esp32"\nCONFIG_SPIRAM=y\nCONFIG_ESP32_REV_MIN_FULL=0\n'
WORKAROUND = "CONFIG_SPIRAM_CACHE_WORKAROUND=y\n"
DUPLDST = "CONFIG_SPIRAM_CACHE_WORKAROUND_STRATEGY_DUPLDST=y\n"


class ClassicPsramSdkconfigTest(unittest.TestCase):
    def run_guard(self, contents, target="esp32", defaults=None, name="sdkconfig"):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = root / name
            if contents is not None:
                config.write_text(contents)
            (root / "sdkconfig.defaults").write_text('CONFIG_IDF_TARGET="esp32"\n')
            output = root / "selected-defaults"
            script = root / "test.cmake"
            script.write_text(
                'cmake_minimum_required(VERSION 3.16)\n'
                + f'set(SDKCONFIG "{config}")\n'
                + (f'set(IDF_TARGET "{target}")\n' if target is not None else "")
                + (f'set(SDKCONFIG_DEFAULTS "{defaults}")\n' if defaults is not None else "")
                + f'include("{GUARD}")\n'
                + "solar_os_enable_classic_psram_workaround()\n" * 2
                + f'file(WRITE "{output}" "${{SDKCONFIG_DEFAULTS}}")\n'
            )
            result = subprocess.run(["cmake", "-P", str(script)], cwd=root,
                                    capture_output=True, text=True)
            return (result, config.read_text() if config.exists() else None,
                    output.read_text().split(";") if output.exists() else [])

    def test_old_strategies_and_disabled_workaround_migrate_without_other_changes(self):
        for strategy in ("MEMW", "NOPS"):
            for enabled in (WORKAROUND, "# CONFIG_SPIRAM_CACHE_WORKAROUND is not set\n"):
                with self.subTest(strategy=strategy, enabled=enabled):
                    before = (CLASSIC + enabled
                              + f"CONFIG_SPIRAM_CACHE_WORKAROUND_STRATEGY_{strategy}=y\n"
                              + "# CONFIG_SPIRAM_CACHE_WORKAROUND_STRATEGY_DUPLDST is not set\n"
                              + "CONFIG_ESP_TASK_WDT_TIMEOUT_S=5\nCONFIG_LOCAL_SETTING=42\n")
                    result, remaining, defaults = self.run_guard(before)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(remaining.count(WORKAROUND), 1)
                    self.assertEqual(remaining.count(DUPLDST), 1)
                    self.assertNotIn(f"CONFIG_SPIRAM_CACHE_WORKAROUND_STRATEGY_{strategy}=y", remaining)
                    self.assertIn("CONFIG_ESP_TASK_WDT_TIMEOUT_S=5\nCONFIG_LOCAL_SETTING=42\n", remaining)
                    self.assertEqual(defaults.count(str(DEFAULTS)), 1)
                    self.assertEqual(result.stderr.count("enabling duplicate load/store"), 1)

    def test_safe_configuration_is_unchanged(self):
        before = CLASSIC + WORKAROUND + DUPLDST
        result, remaining, _ = self.run_guard(before)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(remaining, before)
        self.assertEqual(result.stderr, "")

    def test_fresh_configuration_gets_defaults_without_creating_a_partial_sdkconfig(self):
        result, remaining, defaults = self.run_guard(None)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIsNone(remaining)
        self.assertEqual(defaults, ["sdkconfig.defaults", str(DEFAULTS)])
        self.assertIn(WORKAROUND, DEFAULTS.read_text())
        self.assertIn(DUPLDST, DEFAULTS.read_text())

    def test_preserves_default_profile_order_including_deliberate_repeated_profiles(self):
        result, _, defaults = self.run_guard(None, defaults="first;second;first")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(defaults, ["first", "second", "first", str(DEFAULTS)])

    def test_other_targets_and_classic_without_psram_are_unchanged(self):
        for target in ("esp32s3", "esp32p4", "esp32"):
            before = f'CONFIG_IDF_TARGET="{target}"\n# CONFIG_SPIRAM is not set\n'
            result, remaining, defaults = self.run_guard(before, target)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(remaining, before)
            if target != "esp32":
                self.assertNotIn(str(DEFAULTS), defaults)

    def test_revision_three_and_later_keep_their_configuration(self):
        for revision in (300, 301):
            before = CLASSIC.replace("FULL=0", f"FULL={revision}")
            result, remaining, _ = self.run_guard(before)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(remaining, before)

    def test_legacy_revision_and_target_inference(self):
        before = CLASSIC.replace("CONFIG_ESP32_REV_MIN_FULL=0", "CONFIG_ESP32_REV_MIN=1")
        result, remaining, _ = self.run_guard(before, target=None)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(DUPLDST, remaining)
        result, remaining, _ = self.run_guard(before.replace("REV_MIN=1", "REV_MIN=3"), target=None)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(DUPLDST, remaining)

    def test_does_not_edit_another_targets_stale_configuration(self):
        before = CLASSIC.replace('"esp32"', '"esp32s3"')
        result, remaining, _ = self.run_guard(before)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(remaining, before)

    def test_refuses_to_edit_a_defaults_file_as_generated_configuration(self):
        result, remaining, _ = self.run_guard(CLASSIC, name="sdkconfig.defaults.generated")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not defaults", result.stderr)
        self.assertEqual(remaining, CLASSIC)

    def test_loaded_sdk_configuration_must_include_the_derived_spinlock_workaround(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "check.cmake"
            for spinlock in (False, True):
                script.write_text(
                    f'include("{GUARD}")\n'
                    + "set(CONFIG_IDF_TARGET_ESP32 TRUE)\nset(CONFIG_SPIRAM TRUE)\n"
                    + "set(CONFIG_ESP32_REV_MIN_FULL 0)\n"
                    + "set(CONFIG_SPIRAM_CACHE_WORKAROUND TRUE)\n"
                    + "set(CONFIG_SPIRAM_CACHE_WORKAROUND_STRATEGY_DUPLDST TRUE)\n"
                    + f"set(CONFIG_SPIRAM_WORKAROUND_NEED_VOLATILE_SPINLOCK {spinlock})\n"
                    + "solar_os_check_classic_psram_workaround()\n"
                )
                result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, spinlock, result.stderr)


if __name__ == "__main__":
    unittest.main()
