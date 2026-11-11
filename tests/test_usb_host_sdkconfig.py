from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
GUARD = ROOT / "scripts/usb_host_sdkconfig.cmake"
ENABLED = "CONFIG_USB_HOST_HUBS_SUPPORTED=y\n"
DISABLED = "# CONFIG_USB_HOST_HUBS_SUPPORTED is not set\n"


class UsbHostSdkconfigTest(unittest.TestCase):
    def run_guard(self, contents, defaults=(ENABLED,), config_name="sdkconfig"):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = root / config_name
            if contents is not None:
                config.write_text(contents)
            names = []
            for index, text in enumerate(defaults):
                name = f"sdkconfig.defaults.{index}"
                (root / name).write_text(text)
                names.append(name)
            script = root / "guard.cmake"
            script.write_text(
                f'set(SDKCONFIG "{config.as_posix()}")\n'
                + f'set(SDKCONFIG_DEFAULTS "{";".join(names)}")\n'
                + f'include("{GUARD.as_posix()}")\n'
                + "solar_os_enable_required_usb_hubs()\n" * 2)
            result = subprocess.run(["cmake", "-P", str(script)], cwd=root,
                                    capture_output=True, text=True)
            for name, text in zip(names, defaults):
                self.assertEqual((root / name).read_text(), text)
            return result, config.read_text() if config.exists() else None

    def test_existing_configs_migrate_once_without_losing_local_settings(self):
        for setting in (DISABLED, "CONFIG_USB_HOST_HUBS_SUPPORTED=n\n", ""):
            before = 'CONFIG_IDF_TARGET="esp32s3"\nCONFIG_LOCAL_SETTING=42\n' + setting
            result, remaining = self.run_guard(before)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('CONFIG_IDF_TARGET="esp32s3"\nCONFIG_LOCAL_SETTING=42\n', remaining)
            self.assertEqual(remaining.count(ENABLED), 1)
            self.assertNotIn(DISABLED, remaining)
            self.assertNotIn("CONFIG_USB_HOST_HUBS_SUPPORTED=n", remaining)
            self.assertEqual(result.stderr.count("enabling USB host hubs"), 1)

    def test_current_or_missing_configuration_is_unchanged(self):
        for contents in (None, ENABLED + "CONFIG_LOCAL_SETTING=42\n"):
            result, remaining = self.run_guard(contents)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(remaining, contents)
            self.assertEqual(result.stderr, "")

    def test_classic_defaults_and_later_explicit_override_leave_local_values(self):
        for defaults in (('CONFIG_IDF_TARGET="esp32"\n',),
                         (ENABLED, DISABLED), (ENABLED, "CONFIG_USB_HOST_HUBS_SUPPORTED=n\n")):
            result, remaining = self.run_guard(DISABLED, defaults)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(remaining, DISABLED)
            self.assertEqual(result.stderr, "")

    def test_board_defaults_migrate_all_s3_configurations(self):
        for path in ROOT.glob("sdkconfig.defaults*"):
            contents = path.read_text()
            if 'CONFIG_IDF_TARGET="esp32s3"' in contents:
                with self.subTest(path=path.name):
                    result, remaining = self.run_guard(DISABLED, (contents,))
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(ENABLED, remaining)

    def test_defaults_cannot_be_used_as_generated_configuration(self):
        result, _ = self.run_guard(DISABLED, config_name="sdkconfig.defaults.generated")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not defaults", result.stderr)


if __name__ == "__main__":
    unittest.main()
