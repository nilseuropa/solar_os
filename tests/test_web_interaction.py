"""Exercise the production web event handlers with simulated input and workers."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions


ROOT = Path(__file__).resolve().parents[1]


class WebInteractionTest(unittest.TestCase):
    def test_navigation_editing_cancellation_and_zoom(self):
        source = (ROOT / "src/apps/solar_os_web.c").read_text()
        # Use the app's actual state and geometry, as well as its event handlers.
        types = source[source.index("#define WEB_URL_MAX"):
                       source.index('static const char *TAG')]
        for name in ("web_text_metrics_t", "web_toolbar_layout_t"):
            end = source.index("} " + name + ";") + len("} " + name + ";")
            start = source.rfind("typedef struct {", 0, end)
            types += source[start:end] + "\n"
        functions = production_functions(source, (
            "web_toolbar_layout", "web_request_active", "web_address_window_start",
            "web_text_metrics", "web_wrap_cols_for_gfx", "web_url_supported",
            "web_is_printable", "web_set_status", "web_line_height_at",
            "web_body_height", "web_visible_line_count", "web_max_scroll",
            "web_clamp_scroll", "web_item_for_line", "web_draw_text_clipped",
            "web_draw_nav_button", "web_draw_toolbar", "web_finish_edit",
            "web_focus_address", "web_history_push", "web_current_history_entry",
            "web_history_push_current", "web_stop_worker_for_navigation",
            "web_stop_loading", "web_start_load", "web_drain_events",
            "web_zoom_page", "web_open_address", "web_handle_address_key",
            "web_history_back", "web_history_forward", "web_reload",
            "web_line_at_pointer_y", "web_pointer_event", "web_event",
        ))
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "web_types.inc").write_text(types)
            (directory / "web_interaction.inc").write_text(functions)
            binary = directory / "web-interaction"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(directory), "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src"), "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "include"),
                str(ROOT / "tests/host/web_interaction_test.c"),
                str(ROOT / "src/services/solar_os_text_search.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_app_receives_generic_pointer_events(self):
        source = (ROOT / "src/apps/solar_os_web.c").read_text()
        descriptor = source.split("const solar_os_app_t solar_os_web_app =", 1)[1]
        self.assertIn("SOLAR_OS_APP_FLAG_POINTER_EVENTS", descriptor)


if __name__ == "__main__":
    unittest.main()
