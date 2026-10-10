from pathlib import Path
import subprocess
import tempfile
import unittest


GUARD = Path(__file__).resolve().parents[1] / "patches/nimble/config_guard.cmake"
REQUIRED = (
    "CONFIG_BT_NIMBLE_ENABLED", "CONFIG_BT_NIMBLE_ROLE_CENTRAL",
    "CONFIG_BT_NIMBLE_ROLE_OBSERVER", "CONFIG_BT_NIMBLE_ROLE_PERIPHERAL",
    "CONFIG_BT_NIMBLE_GATT_SERVER", "CONFIG_BT_NIMBLE_DYNAMIC_SERVICE",
)
CURRENT = ("CONFIG_BT_ENABLED=y\n" + "".join(f"{key}=y\n" for key in REQUIRED)
           + "CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=8192\n")


class NimbleSdkconfigTest(unittest.TestCase):
    def run_guard(self, contents, name="sdkconfig.test", explicit=True):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = root / name
            if contents is not None:
                config.write_text(contents)
            script = root / "guard.cmake"
            script.write_text(
                (f'set(SDKCONFIG "{config.as_posix()}")\n' if explicit else "")
                + f'include("{GUARD.as_posix()}")\n'
                + "solar_os_regenerate_stale_sdkconfig()\n"
                + "solar_os_regenerate_stale_sdkconfig()\n"
            )
            result = subprocess.run(
                ["cmake", "-P", str(script)], cwd=root,
                capture_output=True, text=True,
            )
            remaining = config.read_text() if config.exists() else None
            self.assertEqual(sorted(p.name for p in root.iterdir()),
                             sorted(["guard.cmake"] + ([name] if config.exists() else [])))
            return result, remaining

    def test_stale_host_is_removed_once_with_notice(self):
        result, remaining = self.run_guard(
            "CONFIG_BT_ENABLED=y\nCONFIG_BT_BLUEDROID_ENABLED=y\n"
            "# CONFIG_BT_NIMBLE_ENABLED is not set\n"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIsNone(remaining)
        self.assertEqual(result.stderr.count("regenerating"), 1)

    def test_current_disabled_and_missing_configs_are_unchanged(self):
        for content in (None, "# CONFIG_BT_ENABLED is not set\n",
                        CURRENT):
            with self.subTest(content=content):
                result, remaining = self.run_guard(content)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(remaining, content)
                self.assertEqual(result.stdout, "")

    def test_each_missing_or_disabled_requirement_regenerates(self):
        for key in REQUIRED:
            for replacement in ("", f"# {key} is not set\n"):
                with self.subTest(key=key, replacement=replacement):
                    result, remaining = self.run_guard(CURRENT.replace(f"{key}=y\n", replacement))
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIsNone(remaining)
                    self.assertIn(key, result.stderr)
                    self.assertEqual(result.stderr.count("regenerating"), 1)

    def test_default_sdkconfig_path(self):
        result, remaining = self.run_guard("CONFIG_BT_ENABLED=y\n", "sdkconfig", False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIsNone(remaining)

    def test_small_or_missing_host_stack_is_raised_once_preserving_local_settings(self):
        for setting in ("", "CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096\n"):
            with self.subTest(setting=setting):
                content = CURRENT.replace("CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=8192\n", setting)
                content += "CONFIG_LOCAL_SETTING=123\n"
                result, remaining = self.run_guard(content)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr.count("increasing NimBLE host stack"), 1)
                self.assertEqual(remaining.count("CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE="), 1)
                self.assertIn("CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=8192\n", remaining)
                self.assertIn("CONFIG_LOCAL_SETTING=123\n", remaining)
                self.assertTrue(all(f"{key}=y\n" in remaining for key in REQUIRED))

    def test_larger_local_host_stack_is_preserved(self):
        content = CURRENT.replace("STACK_SIZE=8192", "STACK_SIZE=12288")
        result, remaining = self.run_guard(content)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(remaining, content)
        self.assertEqual(result.stderr, "")

    def test_defaults_are_never_removed(self):
        for name in ("sdkconfig.defaults", "sdkconfig.defaults.board"):
            with self.subTest(name=name):
                content = "CONFIG_BT_ENABLED=y\n"
                result, remaining = self.run_guard(content, name)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(remaining, content)

    def test_incompatible_defaults_fail_before_linking(self):
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "overlay.cmake"
            for missing in REQUIRED:
                with self.subTest(missing=missing):
                    script.write_text(
                        f'include("{GUARD.as_posix()}")\n'
                        + "set(CONFIG_BT_ENABLED ON)\n"
                        + "".join(f"set({key} ON)\n" for key in REQUIRED if key != missing)
                        + f'include("{(GUARD.parent / "overlay.cmake").as_posix()}")\n'
                    )
                    result = subprocess.run(
                        ["cmake", "-P", str(script)], capture_output=True, text=True,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(f"requires {missing}=y", result.stderr)


if __name__ == "__main__":
    unittest.main()
