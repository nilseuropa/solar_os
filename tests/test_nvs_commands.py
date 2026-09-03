import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class NvsCommandTests(unittest.TestCase):
    def test_selective_inventory_and_erase_are_wired(self):
        source = (ROOT / "src/shell/solar_os_shell_system.c").read_text()
        shell = (ROOT / "src/apps/solar_os_shell.c").read_text()
        for api in (
            "nvs_entry_find(",
            "nvs_get_used_entry_count(",
            "nvs_find_key(",
            "nvs_erase_key(",
            "nvs_erase_all(",
            "nvs_commit(",
        ):
            self.assertIn(api, source)
        self.assertIn('"status", "list", "erase", "backup", "restore", "clear"', source)
        self.assertIn('"status", "list", "erase", "backup", "restore", "clear"', shell)

    def test_manual_documents_safe_selective_forms(self):
        manual = (ROOT / "doc/manual/commands.md").read_text()
        self.assertIn("nvs list [namespace]", manual)
        self.assertIn("nvs erase <namespace> [key]", manual)
        self.assertIn("Values are never displayed", manual)
        self.assertIn("reboots after a successful change", manual)


if __name__ == "__main__":
    unittest.main()
