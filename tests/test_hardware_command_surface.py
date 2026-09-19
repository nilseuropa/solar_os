from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMPLETION = (ROOT / "src/apps/solar_os_shell.c").read_text(encoding="utf-8")


class HardwareCommandSurfaceTest(unittest.TestCase):
    def test_new_hardware_commands_complete_subcommands(self):
        expected = {
            "gnss": ("list", "status", "power", "fix"),
            "haptic": ("list", "play", "stop"),
            "charger": (
                "list",
                "status",
                "enable",
                "input-limit",
                "current",
                "voltage",
            ),
            "imu": ("list", "sample"),
            "nfc": ("list", "power", "scan"),
        }
        for command, subcommands in expected.items():
            declaration = (
                f"static const char * const {command}_subcommands[]"
            )
            self.assertIn(declaration, COMPLETION)
            start = COMPLETION.index(declaration)
            end = COMPLETION.index("};", start)
            values = COMPLETION[start:end]
            for subcommand in subcommands:
                self.assertIn(f'"{subcommand}"', values)
            self.assertIn(
                f"SHELL_COMPLETION_STATIC(path_{command}, "
                f"{command}_subcommands)",
                COMPLETION,
            )

    def test_power_and_enable_values_complete(self):
        for path in ("gnss_power", "nfc_power", "charger_enable"):
            self.assertIn(
                f"SHELL_COMPLETION_STATIC(path_{path}, on_off_values)",
                COMPLETION,
            )

    def test_empty_registries_have_visible_output(self):
        expected = {
            "gnss": "no GNSS receivers registered",
            "nfc": "no NFC readers registered",
            "imu": "no motion sensors registered",
            "haptic": "no haptic devices registered",
            "charger": "no battery chargers registered",
        }
        for command, message in expected.items():
            source = (
                ROOT / f"src/shell/solar_os_shell_{command}.c"
            ).read_text(encoding="utf-8")
            self.assertIn(f"solar_os_{command}_count() == 0U", source)
            self.assertIn(f'"{message}"', source)


if __name__ == "__main__":
    unittest.main()
