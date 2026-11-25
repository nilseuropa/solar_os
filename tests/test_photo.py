from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PhotoTest(unittest.TestCase):
    def test_application_lifecycle_capture_and_completion(self):
        for rtsp in (0, 1):
            with self.subTest(rtsp=rtsp), tempfile.TemporaryDirectory() as directory:
                binary = Path(directory) / "photo"
                (Path(directory) / "solar_os_display.h").write_text(
                    '#pragma once\n#include "solar_os_gfx.h"\n'
                    '#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 16\n'
                    'esp_err_t solar_os_display_set_high_refresh_override(const char *,bool,uint16_t);\n')
                (Path(directory) / "solar_os_task.h").write_text(
                    f'#include "{ROOT / "src/solar_os_task.h"}"\n')
                subprocess.run([
                    "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wextra",
                    "-Werror", "-Wno-misleading-indentation", "-pthread",
                    "-fsanitize=address,undefined", "-no-pie", "-DpdTRUE=1",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-DCONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=1",
                    f"-DSOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT={rtsp}",
                    "-include", str(ROOT / "tests/host/compat.h"),
                    "-I", directory,
                    "-I", str(ROOT / "tests/host/script_media_stubs"),
                    "-I", str(ROOT / "tests/host/rtsp_stubs"),
                    "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src"),
                    "-I", str(ROOT / "src/services"), "-I", str(ROOT / "src/apps"),
                    "-I", str(ROOT / "components/stb_image/include"),
                    "-idirafter", str(ROOT / "include"),
                    str(ROOT / "tests/host/photo_app_test.c"),
                    str(ROOT / "src/services/solar_os_keys.c"),
                    str(ROOT / "src/services/solar_os_media_widgets.c"),
                    str(ROOT / "src/services/solar_os_gfx_widgets.c"),
                    "-o", str(binary),
                ], check=True)
                subprocess.run([str(binary)], check=True, timeout=20)

    def test_package_registry_and_documentation(self):
        packages = tomllib.loads((ROOT / "packages/solar_os_packages.toml").read_text())
        app = packages["packages"]["app_photo"]
        self.assertEqual(app["capabilities"], ["psram", "gfx"])
        self.assertNotIn("service_camera", app["depends"])
        self.assertNotIn("service_rtsp_client", app["depends"])
        self.assertIn("service_script_media", app["depends"])
        self.assertIn("service_media_widgets", app["depends"])
        self.assertIn("app_photo", packages["groups"]["photo"]["members"])
        full = tomllib.loads((ROOT / "flavors/full.toml").read_text())
        self.assertTrue(full["groups"]["photo"])
        self.assertIn('APP_ENTRY("photo"', (ROOT / "src/apps/solar_os_app_registry.c").read_text())
        self.assertIn("## photo", (ROOT / "doc/manual/apps.md").read_text())
        shell = (ROOT / "src/apps/solar_os_shell.c").read_text()
        self.assertIn("solar_os_photo_complete(tokens", shell)

    def test_full_flavor_gates_local_and_remote_capabilities(self):
        from tests.test_flavor_packages import generate_flavor_config as config
        catalog = config.load_catalog(ROOT / "packages/solar_os_packages.toml")
        _, _, groups, packages = config.load_flavor(ROOT / "flavors/full.toml", catalog)
        for capabilities in ({"psram", "gfx", "wifi"}, {"psram", "gfx"}, {"psram", "wifi"}):
            with self.subTest(capabilities=capabilities):
                _, selected = config.apply_board_capability_pruning(catalog, groups, packages, capabilities)
                self.assertEqual(selected["app_photo"], {"psram", "gfx"} <= capabilities)
                self.assertEqual(selected["service_rtsp_client"], "wifi" in capabilities)
