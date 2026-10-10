import ast
import re
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DESCRIPTOR = (ROOT / "src/apps/solar_os_script_api.inc").read_text()
PYTHON = (ROOT / "src/apps/solar_os_python_media.inc").read_text()
LUA = (ROOT / "src/apps/solar_os_lua_media.inc").read_text()
SERVICE = (ROOT / "src/services/solar_os_script_media.c").read_text()
PACKAGES = tomllib.loads((ROOT / "packages/solar_os_packages.toml").read_text())["packages"]


class ScriptMediaBindingsTest(unittest.TestCase):
    def test_public_surface_has_matching_handlers(self):
        expected = {
            "streams": ("list", "info", "status", "open", "close", "close_all", "read",
                        "write", "read_scalar", "acquire_frame", "frame_info",
                        "frame_data", "frame_save", "release_frame"),
            "camera": ("status", "snapshot", "capture"),
            "rtsp": ("open", "status", "read_frame", "lateness", "close"),
            "image": ("decode", "from_frame", "present"),
        }
        for module, names in expected.items():
            for name in names:
                self.assertIn(f"SOLAR_OS_SCRIPT_API_FUNCTION({module}, {name}, {name});", DESCRIPTOR)
                self.assertIn(f"solaros_{module}_{name}_obj", PYTHON)
                self.assertIn(f"solua_{module}_{name}", LUA)

    def test_script_package_does_not_force_camera_image_or_rtsp(self):
        package = PACKAGES["service_script_media"]
        self.assertEqual(package["depends"], ["service_streams"])
        self.assertIn("services/solar_os_script_media.c", package["sources"])
        for language in ("python", "lua"):
            self.assertIn("service_script_media", PACKAGES[f"app_{language}"]["depends"])
        for module, gate in (("streams", "SCRIPT_MEDIA"), ("camera", "CAMERA"), ("rtsp", "RTSP_CLIENT")):
            self.assertIn(f"#if SOLAR_OS_PACKAGE_SERVICE_{gate}\nSOLAR_OS_SCRIPT_API_MODULE_BEGIN({module});", DESCRIPTOR)

    def test_all_interpreter_teardown_paths_close_media(self):
        for language, prefix in (("python", "python"), ("lua", "solua")):
            main = (ROOT / f"src/apps/solar_os_{language}.c").read_text()
            self.assertEqual(main.count(f"{prefix}_media_destroy();"), 3)
            self.assertIn(f'#include "solar_os_{language}_media.inc"', main)
            self.assertIn(f"while ((__atomic_load_n(&{prefix}_media_session, __ATOMIC_ACQUIRE) != NULL", main)
            self.assertIn(f"{prefix}_imlib_active()", main)
        self.assertIn("solar_os_script_media_close_all(s)", SERVICE)
        self.assertIn("__atomic_load_n(&s->done, __ATOMIC_ACQUIRE)", SERVICE)
        self.assertIn("solar_os_task_delete_external(s->worker)", SERVICE)
        self.assertIn("solar_os_rtsp_client_cancel", SERVICE)
        self.assertNotRegex(SERVICE, r"\b(?:mp|lua)_\w+")

    def test_frames_remain_native_except_explicit_data_copy(self):
        self.assertIn("mp_obj_new_bytes(frame.jpeg.data, frame.jpeg.length)", PYTHON)
        self.assertIn("lua_pushlstring(L, (const char *)frame.jpeg.data, frame.jpeg.length)", LUA)
        for source in (PYTHON, LUA):
            self.assertIn("solar_os_raster_image_decode(frame.jpeg.data, frame.jpeg.length", source)
            self.assertIn("solar_os_script_media_option_valid", source)
        self.assertIn("SOLAR_OS_MEMORY_EXTERNAL_REQUIRED", SERVICE)
        self.assertIn("previous < 0x1fffffffU", SERVICE)
        self.assertIn("SOLAR_OS_SCRIPT_MEDIA_FRAME_MAX", SERVICE)
        self.assertIn("if (slice > 50) slice = 50", SERVICE)

    def test_native_present_retains_queued_images_and_camera_pins_match(self):
        for language, source in (("python", PYTHON), ("lua", LUA)):
            main = (ROOT / f"src/apps/solar_os_{language}.c").read_text()
            self.assertIn("solar_os_raster_image_present(image, gfx", main)
            present = source.split(f"static {'mp_obj_t solaros' if language == 'python' else 'int solua'}_image_present", 1)[1]
            self.assertIn("solar_os_raster_image_retain(image)", present)
            self.assertIn("solar_os_raster_image_release(image)", present)
            for key in ("d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "siod", "sioc", "vsync", "href", "pclk", "xclk", "pwdn"):
                self.assertIn(f'{{"{key}", "{key}", SOLAR_OS_EXPANSION_BINDING_GPIO}}', main)

    def test_manual_pages_and_python_examples(self):
        for language in ("python", "lua"):
            page = (ROOT / f"doc/manual/{language}.media.md").read_text()
            self.assertIn("## Quick reference", page)
            self.assertIn("error_detail", page)
            self.assertIn("lateness", page)
            self.assertIn("RGB565", page)
            if language == "python":
                for script in re.findall(r"```python\n(.*?)```", page, re.S):
                    ast.parse(script)

    def test_save_errors_preserve_filesystem_cause_after_capture_cleanup(self):
        self.assertIn("mp_obj_new_exception_args(&mp_type_OSError, 2, args)", PYTHON)
        self.assertIn("strerror(file_errno)", PYTHON)
        self.assertIn("strerror(file_errno)", LUA)
        for source, prefix in ((PYTHON, "python"), (LUA, "solua")):
            capture = source.split(f"static {'mp_obj_t solaros' if prefix == 'python' else 'int solua'}_camera_capture", 1)[1]
            self.assertLess(capture.index("solar_os_script_media_release"), capture.index(f"{prefix}_media_check_save"))
            self.assertIn("&file_errno", capture)
        self.assertIn("if (file_errno) *file_errno = 0;", SERVICE)


if __name__ == "__main__":
    unittest.main()
