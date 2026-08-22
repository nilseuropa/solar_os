import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
PYTHON_SOURCE = (REPOSITORY / "src/apps/solar_os_python.c").read_text(encoding="utf-8")
LUA_SOURCE = (REPOSITORY / "src/apps/solar_os_lua.c").read_text(encoding="utf-8")
API_DESCRIPTOR = (REPOSITORY / "src/apps/solar_os_script_api.inc").read_text(
    encoding="utf-8"
)
HTTP_HEADER = (REPOSITORY / "src/services/solar_os_http_client.h").read_text(
    encoding="utf-8"
)
HTTP_SOURCE = (REPOSITORY / "src/services/solar_os_http_client.c").read_text(
    encoding="utf-8"
)


class ScriptHttpBindingsTest(unittest.TestCase):
    def test_python_and_lua_register_the_same_http_methods(self):
        for method in (
            "request", "get", "post", "put", "patch", "delete", "head",
            "stream_open", "stream_read", "stream_close", "stream_close_all",
        ):
            self.assertIn(
                f"SOLAR_OS_SCRIPT_API_FUNCTION(http, {method}, {method});",
                API_DESCRIPTOR,
            )

        for source in (PYTHON_SOURCE, LUA_SOURCE):
            self.assertIn('#include "solar_os_script_api.inc"', source)
            self.assertIn("solar_os_http_perform_buffered", source)
        self.assertIn("#if SOLAR_OS_PACKAGE_SERVICE_HTTP_CLIENT", API_DESCRIPTOR)

    def test_stream_bindings_are_bounded_and_have_python_lua_parity(self):
        stream_header = (
            REPOSITORY / "src/services/solar_os_http_stream.h"
        ).read_text(encoding="utf-8")
        stream_source = (
            REPOSITORY / "src/services/solar_os_http_stream.c"
        ).read_text(encoding="utf-8")
        for token in (
            "SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES 2U",
            "SOLAR_OS_HTTP_STREAM_GLOBAL_MAX_HANDLES 4U",
            "SOLAR_OS_HTTP_STREAM_EVENT_DATA_MAX 1024U",
            "SOLAR_OS_HTTP_STREAM_EVENT_COMPLETE",
            "SOLAR_OS_HTTP_STREAM_EVENT_ERROR",
        ):
            self.assertIn(token, stream_header)
        self.assertIn("solar_os_queue_create(HTTP_STREAM_QUEUE_LEN", stream_source)
        self.assertIn("return ESP_ERR_NO_MEM", stream_source)
        self.assertIn("solar_os_task_create_pinned_internal", stream_source)
        for source in (PYTHON_SOURCE, LUA_SOURCE):
            self.assertIn("solar_os_http_stream_session_destroy", source)
            self.assertIn('"deadline_exceeded"', source)

    def test_python_and_lua_return_the_same_response_fields(self):
        for field in (
            "status_code",
            "content_length",
            "bytes_received",
            "duration_ms",
            "truncated",
            "headers_truncated",
            "headers",
            "body",
        ):
            self.assertIn(f'"{field}"', PYTHON_SOURCE)
            self.assertIn(f'"{field}"', LUA_SOURCE)

    def test_python_content_length_uses_small_int_aware_conversion(self):
        self.assertIn(
            'python_dict_store_i64(result,\n'
            '                          "content_length",\n'
            '                          response->response.content_length);',
            PYTHON_SOURCE,
        )
        self.assertNotIn(
            "mp_obj_new_int_from_ll(response->response.content_length)",
            PYTHON_SOURCE,
        )
        self.assertIn("value >= (int64_t)MP_SMALL_INT_MIN", PYTHON_SOURCE)
        self.assertIn("value <= (int64_t)MP_SMALL_INT_MAX", PYTHON_SOURCE)

    def test_buffered_client_is_bounded_and_cancellation_aware(self):
        self.assertIn("SOLAR_OS_HTTP_BUFFERED_DEFAULT_MAX_BODY", HTTP_HEADER)
        self.assertIn("SOLAR_OS_HTTP_BUFFERED_MAX_BODY", HTTP_HEADER)
        self.assertIn("solar_os_http_cancel_fn", HTTP_HEADER)
        self.assertIn("request->options.should_cancel", HTTP_SOURCE)
        self.assertIn("response->body_truncated = true", HTTP_SOURCE)
        self.assertIn("response->headers_truncated = true", HTTP_SOURCE)


if __name__ == "__main__":
    unittest.main()
