from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class AgentRawPromptTest(unittest.TestCase):
    def test_shell_chunks_raw_agent_prompt_within_app_argument_limits(self):
        source = (ROOT / "src/apps/solar_os_shell.c").read_text(encoding="utf-8")
        launcher = source.split("static bool SHELL_NOINLINE shell_launch_raw_agent_ask", 1)[
            1
        ].split("static bool shell_execute_line", 1)[0]
        self.assertIn("solar_os_shell_launch_raw_remainder", launcher)
        self.assertIn("SOLAR_OS_AGENT_APP_RAW_ASK_COMMAND", launcher)
        self.assertIn("SOLAR_OS_APP_ARG_LEN - 1U", launcher)
        self.assertIn("raw_argc < SOLAR_OS_APP_ARG_MAX", launcher)

    def test_agent_reassembles_raw_prompt_without_added_spaces(self):
        source = (ROOT / "src/apps/solar_os_agent_app.c").read_text(
            encoding="utf-8"
        )
        builder = source.split("static esp_err_t agent_app_build_raw_prompt", 1)[
            1
        ].split("static esp_err_t agent_app_build_script", 1)[0]
        self.assertIn("memcpy(agent_app.prompt + used, chunk, len)", builder)
        self.assertNotIn("agent_app.prompt[used++] = ' '", builder)
        self.assertIn("SOLAR_OS_AGENT_PROMPT_MAX", builder)

    def test_raw_prompt_uses_the_agent_request_path(self):
        source = (ROOT / "src/apps/solar_os_agent_app.c").read_text(
            encoding="utf-8"
        )
        start = source.split("static esp_err_t agent_app_start(", 1)[1].split(
            "static void agent_app_stop", 1
        )[0]
        self.assertGreaterEqual(
            start.count("chat_mode || ask_mode || raw_ask_mode"), 3
        )
        self.assertIn("agent_app.events = solar_os_queue_create", start)
        self.assertIn('"agent (%s)\\n"', start)


if __name__ == "__main__":
    unittest.main()
