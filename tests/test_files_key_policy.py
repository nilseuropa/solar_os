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
            "F3 View F4 Edit F5 Copy F6 Move F7 mKdir F8 Delete F9 Zip",
            self.draw_bottom,
        )
        self.assertNotRegex(self.draw_bottom, r"[A-Za-z]-[A-Za-z]")

    def test_help_renders_letter_mnemonics_in_bold(self):
        for label, offset in (
            ("View", 0),
            ("Edit", 0),
            ("Copy", 0),
            ("Move", 0),
            ("mKdir", 1),
            ("Delete", 0),
            ("Zip", 0),
        ):
            self.assertIn(f'{{"{label}", {offset}U}}', self.draw_bottom)
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
            "F7": ("k", "K"),
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

        mkdir_cases = self.event_handler.split(
            "case SOLAR_OS_KEY_F7:", 1
        )[1].split("case SOLAR_OS_KEY_F8:", 1)[0]
        self.assertNotIn("case 'n':", mkdir_cases)
        self.assertNotIn("case 'N':", mkdir_cases)

    def test_j_and_k_do_not_navigate(self):
        up_cases = self.event_handler.split(
            "case SOLAR_OS_KEY_UP:", 1
        )[1].split("case SOLAR_OS_KEY_DOWN:", 1)[0]
        down_cases = self.event_handler.split(
            "case SOLAR_OS_KEY_DOWN:", 1
        )[1].split("case SOLAR_OS_KEY_PAGE_UP:", 1)[0]
        self.assertNotIn("case 'k':", up_cases)
        self.assertNotIn("case 'j':", down_cases)


if __name__ == "__main__":
    unittest.main()
