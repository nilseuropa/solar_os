from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
GUARD = ROOT / "scripts/cxx_sdkconfig.cmake"
ENABLED = "CONFIG_COMPILER_CXX_EXCEPTIONS=y\n"
DISABLED = "# CONFIG_COMPILER_CXX_EXCEPTIONS is not set\n"


class CxxSdkconfigTest(unittest.TestCase):
    def run_guard(self, contents, defaults=ENABLED, explicit_defaults=False,
                  config_name="sdkconfig"):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = root / config_name
            if contents is not None:
                config.write_text(contents)
            default_name = "sdkconfig.defaults.board" if explicit_defaults else "sdkconfig.defaults"
            (root / default_name).write_text(defaults)
            script = root / "guard.cmake"
            script.write_text(
                f'set(SDKCONFIG "{config.as_posix()}")\n'
                + (f'set(SDKCONFIG_DEFAULTS "{default_name}")\n' if explicit_defaults else "")
                + f'include("{GUARD.as_posix()}")\n'
                + "solar_os_enable_required_cxx_exceptions()\n" * 2
            )
            result = subprocess.run(["cmake", "-P", str(script)], cwd=root,
                                    capture_output=True, text=True)
            remaining = config.read_text() if config.exists() else None
            self.assertEqual((root / default_name).read_text(), defaults)
            return result, remaining

    def test_disabled_or_missing_setting_migrates_once_and_preserves_local_values(self):
        for setting in (DISABLED, "CONFIG_COMPILER_CXX_EXCEPTIONS=n\n", ""):
            for explicit in (False, True):
                with self.subTest(setting=setting, board_defaults=explicit):
                    before = "CONFIG_LOCAL_SETTING=42\n" + setting + "CONFIG_OTHER_SETTING=y\n"
                    result, remaining = self.run_guard(before, explicit_defaults=explicit)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(remaining.count(ENABLED), 1)
                    self.assertNotIn(DISABLED, remaining)
                    self.assertNotIn("CONFIG_COMPILER_CXX_EXCEPTIONS=n", remaining)
                    self.assertIn("CONFIG_LOCAL_SETTING=42\n", remaining)
                    self.assertIn("CONFIG_OTHER_SETTING=y\n", remaining)
                    self.assertEqual(result.stderr.count("enabling required C++ exceptions"), 1)

    def test_current_or_missing_configuration_is_unchanged(self):
        for contents in (None, ENABLED + "CONFIG_LOCAL_SETTING=42\n"):
            result, remaining = self.run_guard(contents)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(remaining, contents)
            self.assertEqual(result.stderr, "")

    def test_board_defaults_without_exceptions_leave_configuration_unchanged(self):
        result, remaining = self.run_guard(DISABLED, "CONFIG_IDF_TARGET=\"esp32\"\n", True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(remaining, DISABLED)
        self.assertEqual(result.stderr, "")

    def test_defaults_cannot_be_used_as_generated_configuration(self):
        result, _ = self.run_guard(DISABLED, config_name="sdkconfig.defaults.generated")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not defaults", result.stderr)

    def test_all_s3_defaults_support_exception_runtime(self):
        for path in ROOT.glob("sdkconfig.defaults*"):
            contents = path.read_text()
            if 'CONFIG_IDF_TARGET="esp32s3"' in contents:
                with self.subTest(path=path.name):
                    self.assertIn(ENABLED, contents)
                    self.assertIn("CONFIG_COMPILER_CXX_EXCEPTIONS_EMG_POOL_SIZE=1024\n", contents)


if __name__ == "__main__":
    unittest.main()
