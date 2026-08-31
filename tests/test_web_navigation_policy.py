from pathlib import Path
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
WEB_SOURCE = (REPOSITORY / "src/apps/solar_os_web.c").read_text(encoding="utf-8")


class WebNavigationPolicyTest(unittest.TestCase):
    def test_toolbar_exposes_back_reload_and_forward(self):
        for icon in (
            "SOLAR_OS_GFX_ICON_ARROW_LEFT",
            "SOLAR_OS_GFX_ICON_RELOAD",
            "SOLAR_OS_GFX_ICON_ARROW_RIGHT",
        ):
            self.assertIn(icon, WEB_SOURCE)

    def test_history_is_bidirectional(self):
        self.assertIn("back_history[WEB_HISTORY_COUNT]", WEB_SOURCE)
        self.assertIn("forward_history[WEB_HISTORY_COUNT]", WEB_SOURCE)
        self.assertIn("static bool web_history_back", WEB_SOURCE)
        self.assertIn("static bool web_history_forward", WEB_SOURCE)

    def test_pointer_events_activate_page_items(self):
        pointer_handler = WEB_SOURCE.split(
            "static bool web_pointer_event", 1
        )[1].split("static bool web_search_segment", 1)[0]
        self.assertIn("web_item_for_line", pointer_handler)
        self.assertIn("web_open_selected(ctx)", pointer_handler)
        self.assertIn("SOLAR_OS_INPUT_POINTER_PRESS", pointer_handler)

    def test_completed_load_does_not_force_first_item_selection(self):
        done_handler = WEB_SOURCE.split("case WEB_EVENT_DONE:", 1)[1].split(
            "default:", 1
        )[0]
        self.assertNotIn("web.selected_item = 0", done_handler)

    def test_links_use_the_regular_body_font(self):
        render = WEB_SOURCE.split("static void web_render", 1)[1].split(
            "static bool web_zoom_page", 1
        )[0]
        self.assertIn("line->style == WEB_LINE_HEADING", render)
        self.assertIn("metrics.bold_font : metrics.regular_font", render)
        self.assertNotIn("line->style == WEB_LINE_LINK ?", render)

    def test_zoom_reflows_cached_html_without_loading_images(self):
        zoom = WEB_SOURCE.split("static bool web_zoom_page", 1)[1].split(
            "static void web_history_push", 1
        )[0]
        self.assertIn("web_parse_html();", zoom)
        self.assertIn("web.preserved_image_count = web.image_count", zoom)
        self.assertNotIn("web_load_images", zoom)

    def test_zoom_keys_match_reader(self):
        self.assertIn("case SOLAR_OS_KEY_CTRL_PLUS:", WEB_SOURCE)
        self.assertIn("case SOLAR_OS_KEY_CTRL_MINUS:", WEB_SOURCE)
        self.assertIn("redraw = web_zoom_page(ctx, 1);", WEB_SOURCE)
        self.assertIn("redraw = web_zoom_page(ctx, -1);", WEB_SOURCE)


if __name__ == "__main__":
    unittest.main()
