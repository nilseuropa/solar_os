from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.test_foreground_presenter import production_functions
from tests.test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class HimaxProcessorTest(unittest.TestCase):
    def test_transport_failure_releases_camera_for_recovery(self):
        source = production_functions((ROOT / "src/services/solar_os_native_media.c").read_text(),
                                      ("native_sscma_close",))
        run_c(r'''
#include <assert.h>
#include "solar_os_camera.h"
static esp_err_t stop_error, invalidate_error;
static unsigned stops, invalidations, releases;
static int native_result(esp_err_t error) { return error; }
esp_err_t solar_os_camera_stop(const solar_os_camera_owner_t *token)
{ assert(token->generation==77); ++stops; return stop_error; }
esp_err_t solar_os_camera_invalidate(const solar_os_camera_owner_t *token)
{ assert(token->generation==77 && stops); ++invalidations; return invalidate_error; }
esp_err_t solar_os_camera_release_owner(solar_os_camera_owner_t *token)
{ assert(token->generation==77); ++releases; return ESP_OK; }
''' + source + r'''
int main(void) {
 assert(native_sscma_close(77)==ESP_OK && stops==1 && !invalidations && releases==1);
 stop_error=ESP_ERR_TIMEOUT;
 assert(native_sscma_close(77)==ESP_OK && stops==2 && invalidations==1 && releases==2);
 invalidate_error=ESP_ERR_INVALID_STATE;
 assert(native_sscma_close(77)==ESP_ERR_INVALID_STATE && stops==3 && invalidations==2 && releases==2);
}
''', "-I", str(ROOT / "src/services"), "-I", str(ROOT / "include"))

    def test_provider_pipeline_lifetime_and_failures(self):
        cjson = Path.home() / ".platformio/packages/framework-espidf/components/json/cJSON"
        with tempfile.TemporaryDirectory() as tmp:
            for builtin in (0, 1):
                binary = Path(tmp) / f"himax-processor-{builtin}"
                subprocess.run([
                    "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-pthread",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-no-pie",
                    "-DSOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES=1",
                    f"-DSOLAR_OS_PACKAGE_BACKEND_PROCESSOR_HIMAX={builtin}",
                    "-I" + str(ROOT / "tests/host/pipeline_stubs"),
                    "-I" + str(ROOT / "tests/host/himax_stubs"),
                    "-I" + str(ROOT / "tests/host/rtsp_stubs"),
                    "-I" + str(ROOT / "tests/host"), "-I" + str(ROOT / "src"),
                    "-I" + str(ROOT / "src/services"), "-I" + str(ROOT / "src/drivers"),
                    "-I" + str(ROOT / "include"), "-I" + str(cjson),
                    str(ROOT / "tests/host/himax_processor_test.c"),
                    str(ROOT / "modules/himax/provider.c"),
                    str(ROOT / "src/services/solar_os_processor.c"),
                    str(ROOT / "src/services/solar_os_pipeline.c"),
                    str(ROOT / "src/services/solar_os_media_sample.c"),
                    str(ROOT / "src/services/solar_os_json_scan.c"),
                    str(ROOT / "src/drivers/himax_hx6538.c"),
                    str(cjson / "cJSON.c"), "-lm", "-lcrypto", "-o", str(binary),
                ], check=True)
                subprocess.run([str(binary)], check=True, timeout=60)
