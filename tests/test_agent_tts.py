import pathlib
import unittest


REPOSITORY = pathlib.Path(__file__).resolve().parents[1]
APP = (REPOSITORY / "src/apps/solar_os_agent_app.c").read_text(encoding="utf-8")
SHELL = (REPOSITORY / "src/shell/solar_os_shell_agent.c").read_text(
    encoding="utf-8"
)
MANUAL = (REPOSITORY / "doc/manual/agent.md").read_text(encoding="utf-8")


class AgentTtsTest(unittest.TestCase):
    def test_tts_flag_uses_optional_local_speech_service(self):
        self.assertIn('strcmp(argv[1], "--tts") == 0', SHELL)
        self.assertIn("AGENT_APP_TTS_LOCAL", APP)
        self.assertIn("#if SOLAR_OS_PACKAGE_SERVICE_SPEECH", APP)
        self.assertIn("solar_os_speech_queue_get_status", APP)
        self.assertIn("solar_os_speech_enqueue(&request, NULL)", APP)
        self.assertIn("speechd is not running", APP)
        self.assertNotIn("--tts=", APP)

    def test_only_post_tool_successful_answer_is_spoken(self):
        text_case = APP.split("case SOLAR_OS_AGENT_EVENT_TEXT_DELTA:", 1)[1]
        text_case = text_case.split("case SOLAR_OS_AGENT_EVENT_TOOL_CALL:", 1)[0]
        self.assertIn("agent_app_tts_append(event.text);", text_case)

        tool_case = APP.split("case SOLAR_OS_AGENT_EVENT_TOOL_CALL:", 1)[1]
        tool_case = tool_case.split("case SOLAR_OS_AGENT_EVENT_TOOL_RESULT:", 1)[0]
        self.assertIn("agent_app_tts_reset();", tool_case)
        self.assertNotIn("agent_app_tts_speak", tool_case)

        done_case = APP.split("case SOLAR_OS_AGENT_EVENT_DONE:", 1)[1]
        done_case = done_case.split("default:", 1)[0]
        self.assertIn("if (event.success)", done_case)
        self.assertIn("agent_app_tts_speak(io);", done_case)

        status_case = APP.split("case SOLAR_OS_AGENT_EVENT_STATUS:", 1)[1]
        status_case = status_case.split("case SOLAR_OS_AGENT_EVENT_TEXT_DELTA:", 1)[0]
        self.assertNotIn("agent_app_tts", status_case)

    def test_manual_documents_silent_non_answer_events(self):
        self.assertIn("agent --tts", MANUAL)
        self.assertIn("intermediate model text preceding a tool", MANUAL)
        self.assertIn("call remain silent", MANUAL)
        self.assertIn("currently selects the local PicoTTS speech service", MANUAL)


if __name__ == "__main__":
    unittest.main()
