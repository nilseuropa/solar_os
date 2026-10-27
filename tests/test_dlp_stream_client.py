from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DlpStreamClientTest(unittest.TestCase):
    def test_production_stream_adapter_and_host(self):
        source = (ROOT / "src/drivers/solar_os_display_link.c").read_text()
        state = re.search(r"struct solar_os_display_link \{.*?\n\};", source, re.S).group()
        core = state + "\n" + production_functions(source, (
            "result", "now_us", "connect_endpoint", "solar_os_display_link_open_transport",
            "solar_os_display_link_destroy", "solar_os_display_link_info",
            "solar_os_display_link_format", "solar_os_display_link_present",
            "ensure_graphics", "solar_os_display_link_u8g2"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "u8g2.h").write_text("#pragma once\n"
                "typedef struct { int unused; } u8g2_t;\n"
                "typedef struct { unsigned tile_width, tile_height, pixel_width, pixel_height; } u8x8_display_info_t;\n"
                "#define u8g2_SetupDisplay(...) ((void)0)\n"
                "#define u8g2_SetupBuffer(...) ((void)0)\n"
                "#define u8g2_InitDisplay(...) ((void)0)\n"
                "#define u8g2_ClearBuffer(...) ((void)0)\n")
            (path / "freertos").mkdir()
            (path / "freertos/semphr.h").write_text("#pragma once\n#include <stdlib.h>\n"
                "#define portMAX_DELAY 0xffffffffU\n"
                "typedef int StaticSemaphore_t;\n"
                "typedef int *SemaphoreHandle_t;\n"
                "static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *p) {return p;}\n"
                "static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {return malloc(sizeof(int));}\n"
                "static inline int xSemaphoreTake(SemaphoreHandle_t p, unsigned t) {(void)p;(void)t;return 1;}\n"
                "static inline int xSemaphoreGive(SemaphoreHandle_t p) {(void)p;return 1;}\n"
                "static inline void vSemaphoreDelete(SemaphoreHandle_t p) {free(p);}\n")
            (path / "dlp_client_core.inc").write_text(core)
            binary = path / "test"
            subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I" + directory, "-I" + str(ROOT / "tests/host"),
                "-I" + str(ROOT / "src/drivers"), "-I" + str(ROOT / "src/services"),
                "-I" + str(ROOT / "src"),
                "-I" + str(ROOT / "include"), "-I" + str(ROOT / "protocol/display_link/include"),
                str(ROOT / "tests/host/dlp_stream_client_test.c"),
                str(ROOT / "src/services/solar_os_dlp_stream.c"),
                str(ROOT / "src/services/solar_os_dlp_receiver.c"),
                str(ROOT / "src/services/solar_os_port.c"),
                str(ROOT / "src/services/solar_os_stream.c"),
                str(ROOT / "protocol/display_link/src/sdlp.c"), "-o", str(binary)], check=True)
            # Measure optimized, uninstrumented frames separately: ASan adds
            # its own stack overhead. Discovery nests these frames below the
            # display shell and network stack; they previously used >2 KiB.
            subprocess.run(["cc", "-std=c11", "-Os", "-fstack-usage", "-c",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I" + directory, "-I" + str(ROOT / "tests/host"),
                "-I" + str(ROOT / "src/drivers"), "-I" + str(ROOT / "src/services"),
                "-I" + str(ROOT / "src"), "-I" + str(ROOT / "include"),
                "-I" + str(ROOT / "protocol/display_link/include"),
                str(ROOT / "src/services/solar_os_dlp_stream.c"),
                str(ROOT / "protocol/display_link/src/sdlp.c")], cwd=path, check=True)
            budgets = {"sdlp_host_command": 1024, "request": 192, "send_packet": 192}
            measured = {}
            for usage in path.glob("*.su"):
                if not (usage.name.endswith("sdlp.su") or usage.name.endswith("solar_os_dlp_stream.su")):
                    continue
                for line in usage.read_text().splitlines():
                    location, size, _kind = line.split("\t")
                    name = location.rsplit(":", 1)[-1]
                    if name in budgets:
                        measured[name] = int(size)
                        self.assertLessEqual(int(size), budgets[name], name)
            self.assertEqual(measured.keys(), budgets.keys())
            subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
