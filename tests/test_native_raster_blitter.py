import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GFX_HEADER = (ROOT / "src/services/solar_os_gfx.h").read_text(encoding="utf-8")
GFX_SOURCE = (ROOT / "src/services/solar_os_gfx.c").read_text(encoding="utf-8")
RASTER_IMAGE = (ROOT / "src/services/solar_os_raster_image.c").read_text(
    encoding="utf-8"
)
VIEW = (ROOT / "src/apps/solar_os_view.c").read_text(encoding="utf-8")
WEB = (ROOT / "src/apps/solar_os_web.c").read_text(encoding="utf-8")
DOC = (ROOT / "src/services/solar_os_doc.c").read_text(encoding="utf-8")
READER = (ROOT / "src/apps/solar_os_reader.c").read_text(encoding="utf-8")
SKETCH = (ROOT / "src/apps/solar_os_sketch.c").read_text(encoding="utf-8")


class NativeRasterBlitterTest(unittest.TestCase):
    def test_shared_blitter_supports_gray_and_rgb(self):
        self.assertIn("SOLAR_OS_GFX_RASTER_GRAY8", GFX_HEADER)
        self.assertIn("SOLAR_OS_GFX_RASTER_RGB888", GFX_HEADER)
        self.assertIn("solar_os_gfx_blit_raster", GFX_HEADER)
        self.assertIn("gfx_index8_for_rgb888", GFX_SOURCE)
        self.assertIn("gfx_pattern_draw_color", GFX_SOURCE)
        self.assertIn("gfx_set_mono_pixel_raw", GFX_SOURCE)

    def test_all_decoded_raster_consumers_use_shared_blitter(self):
        for source in (RASTER_IMAGE, VIEW, WEB, DOC):
            self.assertIn("solar_os_gfx_blit_raster", source)

        self.assertNotIn("view_image_pixel_color", VIEW)
        self.assertNotIn("web_image_pixel_color", WEB)
        self.assertNotIn("doc_gray_to_color", DOC)

    def test_decoder_inventory_is_covered_and_reader_uses_document_renderer(self):
        decoder_consumers = {
            path.relative_to(ROOT).as_posix()
            for path in (ROOT / "src").rglob("*.c")
            if "solar_os_stb_decode" in path.read_text(encoding="utf-8")
            or "solar_os_webp_decode" in path.read_text(encoding="utf-8")
        }
        self.assertEqual(
            decoder_consumers,
            {
                "src/apps/solar_os_view.c",
                "src/apps/solar_os_web.c",
                "src/apps/solar_os_sketch.c",
                "src/services/solar_os_doc.c",
                "src/services/solar_os_raster_image.c",
            },
        )
        self.assertIn("solar_os_doc_set_asset_provider", READER)
        self.assertIn("solar_os_gfx_blit_raster", DOC)
        self.assertIn("solar_os_gfx_bitmap_2bpp", SKETCH)


if __name__ == "__main__":
    unittest.main()
