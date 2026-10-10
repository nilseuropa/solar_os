from pathlib import Path
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CameraExpansionTest(unittest.TestCase):
    def test_camera_backend_does_not_require_fitted_camera_capability(self):
        packages = tomllib.loads((ROOT / "packages/solar_os_packages.toml").read_text())["packages"]
        for name in ("service_camera", "driver_camera_esp32", "job_cam_webd"):
            self.assertNotIn("camera", packages[name]["capabilities"])
            self.assertEqual(packages[name]["targets"], ["esp32s3"])
        self.assertEqual(packages["driver_camera_esp32"]["expansion_drivers"],
                         ["solar_os_camera_esp32_expansion_driver"])

    def test_binding_catalog_matches_reusable_driver(self):
        catalog = tomllib.loads((ROOT / "boards/expansion_drivers.toml").read_text())
        bindings = catalog["drivers"]["esp32-camera"]["bindings"]
        expected = [f"d{i}" for i in range(8)] + ["siod", "sioc", "vsync", "href", "pclk", "xclk", "pwdn", "reset", "i2c"]
        self.assertEqual([entry["key"] for entry in bindings], expected)
        required = {entry["key"] for entry in bindings if entry["required"]}
        self.assertEqual(required, {f"d{i}" for i in range(8)} |
                         {"vsync", "href", "pclk", "xclk"})
        source = (ROOT / "src/drivers/solar_os_camera_esp32.c").read_text()
        self.assertNotIn("SOLAR_OS_BOARD_PIN_CAMERA_", source)
        self.assertIn("SOLAR_OS_MEMORY_EXTERNAL_PREFERRED", source)
        self.assertIn("SOLAR_OS_RESOURCE_CAMERA_PORT", source)
        self.assertNotIn("solar_os_camera_esp32_register", (ROOT / "src/main.c").read_text())

    def test_memory_mapping_leaves_capture_dma_and_isr_storage_internal(self):
        fragment = (ROOT / "src/camera_memory.lf").read_text()
        entries = [line.strip() for line in fragment.splitlines()
                   if line.strip() and not line.startswith("#")]
        self.assertIn("dram -> flash_rodata", entries)
        self.assertIn("sccb-ng:devices (solar_camera_sccb_devices)", entries)
        self.assertFalse(any(line.startswith("cam_hal (") or line.startswith("ll_cam (")
                             or line.startswith("* (") for line in entries))


if __name__ == "__main__":
    unittest.main()
