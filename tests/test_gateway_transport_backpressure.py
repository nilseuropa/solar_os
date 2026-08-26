import re
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
TRANSPORT = (
    REPOSITORY / "src/services/solar_os_chat_transport_gateway.c"
).read_text(encoding="utf-8")


class GatewayTransportBackpressureTest(unittest.TestCase):
    def test_full_event_queue_backpressures_replay_until_shutdown(self):
        self.assertIn("#define CHAT_EVENT_QUEUE_LEN 12", TRANSPORT)
        self.assertIn("#define CHAT_EVENT_QUEUE_WAIT_MS 20U", TRANSPORT)

        function = re.search(
            r"static bool chat_queue_event_owned\([^)]*\)\s*\{(.*?)\n\}",
            TRANSPORT,
            re.DOTALL,
        )
        self.assertIsNotNone(function)
        body = function.group(1)
        self.assertIn("while (!chat_state.stop_requested)", body)
        self.assertIn("pdMS_TO_TICKS(CHAT_EVENT_QUEUE_WAIT_MS)", body)
        self.assertNotRegex(body, r"xQueueSend\([^;]*,\s*0\s*\)")
        self.assertNotIn("chat_count_dropped_event();", body)


if __name__ == "__main__":
    unittest.main()
