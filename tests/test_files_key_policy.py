from pathlib import Path
import re
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
FILES_SOURCE = (REPOSITORY / "src/apps/solar_os_files.c").read_text(
    encoding="utf-8"
)


class FilesKeyPolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.draw_bottom = FILES_SOURCE.split(
            "static void files_draw_bottom", 1
        )[1].split("static void files_render", 1)[0]
        cls.event_handler = FILES_SOURCE.split(
            "static bool files_event", 1
        )[1].split("static bool files_state_release_ready", 1)[0]

    def test_function_keys_remain_primary_and_help_shows_letter_mnemonics(self):
        self.assertIn(
            "F3 V-iew F4 E-dit F5 C-opy F6 M-ove F7 mK-dir F8 D-elete F9 Z-ip",
            self.draw_bottom,
        )

    def test_help_renders_letter_mnemonics_in_bold(self):
        self.assertIn('strchr("VECMKDZ", help[col])', self.draw_bottom)
        self.assertRegex(
            self.draw_bottom,
            re.compile(
                r"SOLAR_OS_TUI_ATTR_INVERSE\s*\|\s*"
                r"SOLAR_OS_TUI_ATTR_BOLD"
            ),
        )

    def test_file_operations_have_letter_aliases(self):
        aliases = {
            "F3": ("v", "V"),
            "F4": ("e", "E"),
            "F5": ("c", "C"),
            "F6": ("m", "M"),
            "F7": ("K",),
            "F8": ("d", "D"),
            "F9": ("z", "Z"),
        }
        for function_key, letters in aliases.items():
            with self.subTest(function_key=function_key):
                self.assertRegex(
                    self.event_handler,
                    re.compile(
                        rf"case SOLAR_OS_KEY_{function_key}:.*?"
                        + ".*?".join(rf"case '{letter}':" for letter in letters),
                        re.DOTALL,
                    ),
                )

    def test_lowercase_k_remains_cursor_up(self):
        self.assertRegex(
            self.event_handler,
            re.compile(
                r"case SOLAR_OS_KEY_UP:\s*case 'k':\s*"
                r"files_move_cursor\(pane, -1\);"
            ),
        )


if __name__ == "__main__":
    unittest.main()
