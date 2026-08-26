import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]


def source(path: str) -> str:
    return (REPOSITORY / path).read_text(encoding="utf-8")


class GatewayReplayProtocolTest(unittest.TestCase):
    def test_transport_exchanges_numeric_message_ids_and_room_cursors(self):
        transport = source("src/services/solar_os_chat_transport_gateway.c")
        self.assertIn(
            'solar_os_json_scan_object_uint64(line, "id", &event->message_key)',
            transport,
        )
        self.assertIn('\\"cursor\\":%" PRIu64', transport)
        self.assertIn("command->cursor", transport)

    def test_rejoin_uses_desired_rooms_and_retained_room_cursor(self):
        sync = source("src/jobs/solar_os_gateway_sync_job.c")
        self.assertIn(".desired", sync)
        self.assertIn("solar_os_chat_channel_cursor(", sync)
        self.assertGreaterEqual(sync.count("solar_os_chat_channel_cursor("), 2)
        self.assertNotIn("cursor_endpoint", sync)

    def test_join_is_confirmed_only_by_server_event(self):
        service = source("src/services/solar_os_chat.c")
        join_start = service.index("esp_err_t solar_os_chat_join(")
        join_end = service.index("esp_err_t solar_os_chat_leave(", join_start)
        join_body = service[join_start:join_end]
        self.assertIn("chat_add_channel_locked(channel, true, false)", join_body)
        self.assertNotIn("chat_add_channel_locked(channel, true, true)", join_body)
        self.assertIn("chat.channels[existing_index].desired", join_body)

        publish_start = service.index("esp_err_t solar_os_gateway_sync_publish(")
        publish_body = service[publish_start:]
        self.assertIn("SOLAR_OS_CHAT_EVENT_JOINED", publish_body)
        self.assertIn("chat_add_channel_locked(event->channel, false, true)", publish_body)
        self.assertIn("SOLAR_OS_CHAT_EVENT_DISCONNECTED", publish_body)
        self.assertIn("if (!connected)", service)

        app = source("src/apps/solar_os_chat.c")
        self.assertIn("chat_check_pending_gateway_join();", app)
        self.assertIn('chat_set_status("waiting for join confirmation")', app)
        self.assertIn("chat_app.tab = CHAT_APP_TAB_CHAT", app)

    def test_native_chatd_replays_only_messages_newer_than_cursor(self):
        chatd = source("src/jobs/solar_os_chatd_job.c")
        self.assertIn(
            'solar_os_json_scan_object_uint64(line, "cursor", &cursor)',
            chatd,
        )
        self.assertIn("entry->message_id <= cursor", chatd)
        self.assertIn('",\\\"id\\\":"', chatd)
        self.assertNotIn(
            "chatd_client_join(state, client_index, CHATD_DEFAULT_CHANNEL",
            chatd,
        )


if __name__ == "__main__":
    unittest.main()
