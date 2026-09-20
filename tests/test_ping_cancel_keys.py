from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
NETWORK = (ROOT / "src/shell/solar_os_shell_network.c").read_text(
    encoding="utf-8"
)


class PingCancelKeysTest(unittest.TestCase):
    def test_ping_accepts_escape_ctrl_c_and_app_exit(self):
        matcher = NETWORK.split("static bool shell_stop_key_matches", 1)[1]
        matcher = matcher.split("static bool shell_read_stop_key", 1)[0]

        self.assertIn("SOLAR_OS_KEY_APP_EXIT", matcher)
        self.assertIn("SOLAR_OS_KEY_ESCAPE", matcher)
        self.assertIn("ch == 0x03U", matcher)
        self.assertIn("raw_port && ch == 0x1dU", matcher)

    def test_only_ping_enables_the_extra_stop_keys(self):
        self.assertIn("return shell_read_stop_key(user, true);", NETWORK)
        self.assertIn("return shell_read_stop_key(user, false);", NETWORK)
        self.assertIn("ping_read_stop_key,", NETWORK)
        self.assertIn("if (shell_read_app_exit_key(term))", NETWORK)

    def test_ping_prompts_list_the_common_stop_keys(self):
        self.assertEqual(NETWORK.count("Esc, Ctrl+C, or %s"), 2)


if __name__ == "__main__":
    unittest.main()
