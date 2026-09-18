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
        cls.quit_request = EDIT_SOURCE.split(
            "static bool editor_request_quit", 1
        )[1].split("static bool editor_handle_quit_confirmation", 1)[0]
        cls.quit_confirmation = EDIT_SOURCE.split(
            "static bool editor_handle_quit_confirmation", 1
        )[1].split("static void editor_open_empty", 1)[0]

    def test_text_and_hex_share_save_shortcuts(self):
        discard_pattern = re.compile(
            r"case SOLAR_OS_KEY_ESCAPE:\s*"
            r"case 0x11:\s*"
            r"(?:case SOLAR_OS_KEY_F10:\s*)?"
            r"solar_os_context_finish\(ctx, 0, NULL\);"
        )
        save_pattern = re.compile(
            r"case 0x13:\s*"
            r"(?:case SOLAR_OS_KEY_F2:\s*)?"
            r"\(void\)editor_save\(\);"
        )
        for handler in (self.text_handler, self.hex_handler):
            self.assertRegex(handler, save_pattern)
        self.assertRegex(self.hex_handler, discard_pattern)

    def test_text_editor_function_keys_alias_common_actions(self):
        self.assertRegex(
            self.text_handler,
            re.compile(
                r"case SOLAR_OS_KEY_ESCAPE:\s*"
                r"case 0x11:\s*"
                r"case SOLAR_OS_KEY_F10:\s*"
                r"if \(!editor_request_quit\(ctx\)\)"
            ),
        )
        self.assertRegex(
            self.text_handler,
            re.compile(
                r"case 0x06:\s*"
                r"case SOLAR_OS_KEY_F3:\s*"
                r"solar_os_text_search_begin_input\(&editor.search\);"
            ),
        )
        self.assertRegex(
            self.text_handler,
            re.compile(
                r"case 0x13:\s*"
                r"case SOLAR_OS_KEY_F2:\s*"
                r"\(void\)editor_save\(\);"
            ),
        )

    def test_dirty_text_editor_prompts_to_save_or_discard_before_exit(self):
        self.assertIn(
            "editor.mode == EDITOR_MODE_TEXT && editor.dirty",
            self.quit_request,
        )
        self.assertIn("editor.quit_pending = true;", self.quit_request)
        self.assertIn("Save changes? Y save / N discard", EDIT_SOURCE)
        self.assertIn(
            "editor.search.input_active || editor.quit_pending ? 1U : 0U",
            EDIT_SOURCE,
        )
        self.assertRegex(
            self.quit_confirmation,
            re.compile(
                r"key == 'y'.*?editor_save\(\) == ESP_OK.*?"
                r"solar_os_context_finish\(ctx, 0, NULL\);",
                re.DOTALL,
            ),
        )
        self.assertRegex(
            self.quit_confirmation,
            re.compile(
                r"key == 'n'.*?solar_os_context_finish\(ctx, 0, NULL\);",
                re.DOTALL,
            ),
        )
        self.assertIn(
            "if (editor_handle_quit_confirmation(ctx, (uint8_t)ch))",
            self.text_handler,
        )
        self.assertRegex(
            self.text_handler,
            re.compile(
                r"SOLAR_OS_KEY_APP_EXIT\).*?editor_request_quit\(ctx\)",
                re.DOTALL,
            ),
        )

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
