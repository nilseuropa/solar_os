import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DESCRIPTOR = (ROOT / "src/apps/solar_os_script_api.inc").read_text(
    encoding="utf-8"
)
PYTHON = (ROOT / "src/apps/solar_os_python.c").read_text(encoding="utf-8")
LUA = (ROOT / "src/apps/solar_os_lua.c").read_text(encoding="utf-8")
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(encoding="utf-8")
SERVICE = (ROOT / "src/services/solar_os_raster_image.c").read_text(
    encoding="utf-8"
)


class ScriptImageBindingsTest(unittest.TestCase):
    def test_image_service_owns_decode_and_draw(self):
        self.assertIn('sources = ["services/solar_os_raster_image.c"]', PACKAGES)
        self.assertIn("solar_os_stb_decode_rgb", SERVICE)
        self.assertIn("solar_os_webp_decode_rgb", SERVICE)
        self.assertIn("solar_os_raster_image_draw", SERVICE)
        self.assertIn("solar_os_gfx_blit_raster", SERVICE)

    def test_python_and_lua_share_image_api(self):
        for entry in (
            "SOLAR_OS_SCRIPT_API_FUNCTION(image, open, open);",
            "SOLAR_OS_SCRIPT_API_FUNCTION(image, load, open);",
            "SOLAR_OS_SCRIPT_API_FUNCTION(image, size, size);",
            "SOLAR_OS_SCRIPT_API_FUNCTION(image, draw, draw);",
            "SOLAR_OS_SCRIPT_API_FUNCTION(image, close, close);",
            "SOLAR_OS_SCRIPT_API_FUNCTION(image, close_all, close_all);",
        ):
            self.assertIn(entry, DESCRIPTOR)
        self.assertIn("#if SOLAR_OS_PACKAGE_SERVICE_IMAGE", DESCRIPTOR)
        for source, prefix in ((PYTHON, "PYTHON"), (LUA, "SOLUA")):
            self.assertIn(f"{prefix}_EVENT_IMAGE_DRAW", source)
            self.assertIn("solar_os_raster_image_open", source)
            self.assertIn("solar_os_raster_image_retain", source)
            self.assertIn("solar_os_raster_image_release", source)

    def test_queued_draw_references_are_released_on_all_paths(self):
        self.assertIn("python_image_release_pending_events", PYTHON)
        self.assertIn("solua_image_release_pending_events", LUA)
        self.assertIn("python_image_close_all", PYTHON)
        self.assertIn("solua_image_close_all_handles", LUA)
        self.assertIn("if (!python_send_event(&event))", PYTHON)
        self.assertIn("if (!solua_send_event(&event))", LUA)


if __name__ == "__main__":
    unittest.main()
