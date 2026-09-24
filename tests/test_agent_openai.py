from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class AgentOpenAITest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (
            ROOT / "src/services/solar_os_agent_openai.c"
        ).read_text(encoding="utf-8")

    def test_all_openai_requests_disable_parallel_tool_calls(self):
        build_body = self.source.split(
            "static esp_err_t agent_openai_build_body", 1
        )[1].split("static esp_err_t agent_openai_error_message", 1)[0]
        self.assertEqual(build_body.count('"parallel_tool_calls\\\":false}'), 5)

    def test_chat_stream_rejects_a_second_tool_call_index(self):
        parser = self.source.split(
            "static esp_err_t agent_openai_parse_tool_delta", 1
        )[1].split("static esp_err_t agent_openai_parse_chat_data", 1)[0]
        self.assertIn(
            'solar_os_json_get_path_uint32(call, "index", &call_index)',
            parser,
        )
        self.assertIn("stream->tool_call_index != call_index", parser)
        self.assertIn("stream->too_many_tools = true", parser)
        self.assertIn("return ESP_ERR_NOT_SUPPORTED", parser)


if __name__ == "__main__":
    unittest.main()
