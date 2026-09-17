from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
GFX_SOURCE = ROOT / "src" / "services" / "solar_os_gfx.c"


class GfxBitmapSemanticsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = GFX_SOURCE.read_text(encoding="utf-8")
        start = cls.source.index("void solar_os_gfx_bitmap(")
        end = cls.source.index("void solar_os_gfx_bitmap_2bpp(", start)
        cls.bitmap = cls.source[start:end]

    def test_xbm_zero_bits_are_transparent(self) -> None:
        self.assertIn("u8g2_SetBitmapMode(gfx->u8g2, 1);", self.source)

    def test_intermediate_bitmap_colors_use_ordered_dithering(self) -> None:
        self.assertIn("const uint8_t threshold = gfx_dither_threshold", self.bitmap)
        self.assertIn("threshold > 0U && threshold < 16U", self.bitmap)
        self.assertIn("gfx_draw_hline_shade_clipped", self.bitmap)


if __name__ == "__main__":
    unittest.main()
