import subprocess
import tempfile
import unittest
from pathlib import Path

from test_foreground_presenter import production_functions


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
        sync = source("src/services/solar_os_gateway_sync.c")
        self.assertIn(".desired", sync)
        self.assertIn("solar_os_chat_channel_cursor(", sync)
        self.assertGreaterEqual(sync.count("solar_os_chat_channel_cursor("), 2)
        self.assertNotIn("cursor_endpoint", sync)

    def test_join_is_confirmed_only_by_server_event(self):
        service = source("src/services/solar_os_chat.c")
        join_start = service.index("static esp_err_t chat_change_membership(")
        join_end = service.index("esp_err_t solar_os_chat_join(", join_start)
        join_body = service[join_start:join_end]
        self.assertIn("chat_add_channel_locked(channel, true, false)", join_body)
        self.assertNotIn("chat_add_channel_locked(channel, true, true)", join_body)
        self.assertIn("chat.channels[index].desired = joining", join_body)

        publish_start = service.index("esp_err_t solar_os_gateway_sync_publish(")
        publish_body = service[publish_start:]
        self.assertIn("SOLAR_OS_CHAT_EVENT_JOINED", publish_body)
        self.assertIn("chat_add_channel_locked(event->channel, false, true)", publish_body)
        self.assertIn("SOLAR_OS_CHAT_EVENT_DISCONNECTED", publish_body)
        self.assertIn("if (!connected)", service)

        app = source("src/apps/solar_os_chat.c")
        self.assertIn("chat_check_pending_join();", app)
        self.assertIn('chat_set_status("waiting for join confirmation")', app)
        self.assertIn("chat_app.tab = CHAT_APP_TAB_CHAT", app)

    def test_supervisor_serializes_start_and_retries_shutdown(self):
        manager = source("src/services/solar_os_gateway_sync.c")
        start = manager.index("static struct {\n    bool starting;")
        end = manager.index("static const char *TAG", start)
        routines = production_functions(manager, (
            "gateway_manager_lock", "solar_os_gateway_sync_start",
            "solar_os_gateway_sync_running", "solar_os_gateway_sync_stop"))
        host = REPOSITORY / "tests/host"
        services = REPOSITORY / "src/services"
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, "gateway_sync_manager.inc").write_text(manager[start:end] + routines)
            binary = Path(directory, "gateway_sync")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                "-DSOLAR_OS_PACKAGE_SERVICE_GATEWAY_SYNC=1",
                "-I", directory, "-I", str(host), "-I", str(REPOSITORY / "include"),
                "-I", str(REPOSITORY / "src"), "-I", str(services),
                str(host / "gateway_sync_worker_test.c"), str(services / "solar_os_native_messages.c"),
                "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

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
