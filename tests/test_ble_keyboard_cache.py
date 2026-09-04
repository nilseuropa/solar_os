from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class BleKeyboardCacheTest(unittest.TestCase):
    def test_forget_cleans_gatt_cache_before_removing_bond(self):
        source = (ROOT / "src/services/solar_os_ble_keyboard.c").read_text(
            encoding="utf-8"
        )
        start = source.index("static void remove_deferred_bonds(void)\n{")
        end = source.index("\nstatic void complete_deferred_bond_forget(void)", start)
        forget = source[start:end]

        cache_clean = "esp_ble_gattc_cache_clean(bdas[i])"
        remove_bond = "esp_ble_remove_bond_device(bdas[i])"
        self.assertIn(cache_clean, forget)
        self.assertIn(remove_bond, forget)
        self.assertLess(forget.index(cache_clean), forget.index(remove_bond))


if __name__ == "__main__":
    unittest.main()
