import tomllib
import unittest
from pathlib import Path
from script_sources import script_source


REPOSITORY = Path(__file__).resolve().parents[1]
PYTHON_SOURCE = script_source(REPOSITORY / "src/apps/solar_os_python.c")
LUA_SOURCE = script_source(REPOSITORY / "src/apps/solar_os_lua.c")
NET_HEADER = (REPOSITORY / "src/services/solar_os_net_session.h").read_text(
    encoding="utf-8"
)
NET_SOURCE = (REPOSITORY / "src/services/solar_os_net_session.c").read_text(
    encoding="utf-8"
)
PACKAGE_SOURCE = (REPOSITORY / "packages/solar_os_packages.toml").read_text(
    encoding="utf-8"
)
API_DESCRIPTOR = (REPOSITORY / "src/apps/solar_os_script_api.inc").read_text(
    encoding="utf-8"
)


class ScriptNetBindingsTest(unittest.TestCase):
    def test_python_and_lua_register_the_same_managed_methods(self):
        methods = (
            "tcp_connect",
            "tcp_send",
            "tcp_receive",
            "udp_open",
            "udp_send",
            "udp_receive",
            "websocket_connect",
            "websocket_send",
            "websocket_receive",
            "close",
            "close_all",
            "limits",
        )
        for method in methods:
            self.assertIn(
                f"SOLAR_OS_SCRIPT_API_FUNCTION(net, {method}, {method});",
                API_DESCRIPTOR,
            )

        for source in (PYTHON_SOURCE, LUA_SOURCE):
            self.assertIn('#include "solar_os_script_api.inc"', source)
            self.assertIn("solar_os_net_session_", source)
        self.assertIn("#if SOLAR_OS_PACKAGE_SERVICE_NET", API_DESCRIPTOR)

    def test_python_and_lua_return_the_same_message_and_limit_fields(self):
        fields = (
            "data",
            "address",
            "port",
            "truncated",
            "datagram_bytes",
            "type",
            "final",
            "closed",
            "frame_bytes",
            "owner",
            "open_channels",
            "session_channels",
            "global_open_channels",
            "global_channels",
            "max_transfer_bytes",
            "max_udp_bytes",
            "max_timeout_ms",
            "poll_slice_ms",
            "synchronous",
        )
        for field in fields:
            self.assertIn(f'"{field}"', PYTHON_SOURCE)
            self.assertIn(f'"{field}"', LUA_SOURCE)

    def test_service_defines_ownership_quotas_and_bounded_blocking(self):
        for constant in (
            "SOLAR_OS_NET_SESSION_MAX_CHANNELS 4U",
            "SOLAR_OS_NET_GLOBAL_MAX_CHANNELS 8U",
            "SOLAR_OS_NET_MAX_TRANSFER_BYTES (64U * 1024U)",
            "SOLAR_OS_NET_MAX_UDP_BYTES 65507U",
            "SOLAR_OS_NET_MAX_TIMEOUT_MS 60000U",
            "SOLAR_OS_NET_POLL_SLICE_MS 50U",
        ):
            self.assertIn(constant, NET_HEADER)

        self.assertIn("generation-checked", NET_HEADER)
        self.assertIn("net_global_open_channels", NET_SOURCE)
        self.assertIn("net_cancelled(session)", NET_SOURCE)
        self.assertIn("net_wait_fd", NET_SOURCE)
        self.assertIn("solar_os_net_session_close_all(session);", NET_SOURCE)
        self.assertIn("net_remaining_ms(deadline_us)", NET_SOURCE)

    def test_both_interpreters_close_channels_before_vm_teardown(self):
        for source, close_network, close_vm in (
            (PYTHON_SOURCE, "python_net_destroy();", "mp_embed_deinit();"),
            (LUA_SOURCE, "solua_net_destroy();", "lua_close(L);"),
        ):
            vm_runs = source.split(close_vm)[:-1]
            self.assertEqual(len(vm_runs), 2)
            for run in vm_runs:
                self.assertIn(close_network, run)

    def test_shared_transport_dependencies(self):
        packages = tomllib.loads(PACKAGE_SOURCE)["packages"]
        service_net = packages["service_net"]
        script_net = packages["service_script_net"]
        self.assertNotIn("services/solar_os_net_session.c", service_net["sources"])
        self.assertNotIn("tcp_transport", service_net["requires"])
        self.assertIn("service_net", script_net["depends"])
        self.assertIn("services/solar_os_net_session.c", script_net["sources"])
        self.assertIn("services/solar_os_tls.c", script_net["sources"])
        self.assertIn("tcp_transport", script_net["requires"])
        services = tomllib.loads((REPOSITORY / "api/native/services.toml").read_text())["service"]
        net = next(service for service in services if service["id"] == "net")
        self.assertEqual(net["gate"], "SOLAR_OS_PACKAGE_SERVICE_NET")
        for consumer in ("app_python", "app_lua", "app_git", "service_osc"):
            self.assertIn("service_script_net", packages[consumer]["depends"])
