from pathlib import Path
import re
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
EDIT_SOURCE = (REPOSITORY / "src/apps/solar_os_edit.c").read_text(encoding="utf-8")


class EditKeyPolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.hex_handler = EDIT_SOURCE.split(
            "static bool editor_hex_event", 1
        )[1].split("static bool edit_event", 1)[0]
        cls.text_handler = EDIT_SOURCE.split(
            "static bool edit_event", 1
        )[1].split("const solar_os_app_t solar_os_edit_app", 1)[0]

    def test_text_and_hex_share_save_and_discard_shortcuts(self):
        discard_pattern = re.compile(
            r"case SOLAR_OS_KEY_ESCAPE:\s*"
            r"case 0x11:\s*"
            r"solar_os_context_finish\(ctx, 0, NULL\);"
        )
        for handler in (self.text_handler, self.hex_handler):
            self.assertRegex(handler, discard_pattern)
            self.assertIn("case 0x13:\n        (void)editor_save();", handler)

    def test_clipboard_shortcuts_remain_assigned(self):
        for handler in (self.text_handler, self.hex_handler):
            self.assertIn("case 0x03:\n        editor_copy_selection();", handler)
            self.assertIn("case 0x16:\n        editor_paste_clipboard();", handler)
            self.assertIn("case 0x18:\n        editor_cut_selection();", handler)

    def test_startup_errors_return_without_an_error_screen(self):
        self.assertNotIn("error_only", EDIT_SOURCE)
        start = EDIT_SOURCE.split("static esp_err_t edit_start", 1)[1].split(
            "static void edit_stop", 1
        )[0]
        self.assertIn("solar_os_context_finish", start)


if __name__ == "__main__":
    unittest.main()
