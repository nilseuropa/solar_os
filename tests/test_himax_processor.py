from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.test_foreground_presenter import production_functions
from tests.test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class HimaxProcessorTest(unittest.TestCase):
    def test_owned_maintenance_transport_bounds_dma_lifetime_and_failures(self):
        source = production_functions((ROOT / "src/services/solar_os_himax_hx6538.c").read_text(),
                                      ("maintenance_control",))
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "solar_os_sscma.h"
#define SOLAR_OS_MEMORY_DMA 2
#define portMAX_DELAY 0xffffffff
typedef struct {size_t length;const void *tx_buffer;void *rx_buffer;} spi_transaction_t;
typedef struct {void *spi;bool have_reset,started,ready;size_t transfer_max;uint8_t *maintenance;} himax_device_t;
static unsigned acquired,released,sent,allocated,invalidated,prepared;
static bool deny;
static esp_err_t acquire_error,transfer_error;
static const uint8_t *expected;
static size_t expected_size;
static void *solar_os_memory_alloc(size_t n,int kind,const char *tag)
{assert(n==528&&kind==2&&tag);if(deny)return NULL;allocated++;return malloc(n);}
static void camera_invalidate(himax_device_t *d){invalidated++;free(d->maintenance);d->maintenance=NULL;d->started=d->ready=false;}
static esp_err_t prepare(himax_device_t *d){prepared++;d->ready=true;return 0;}
static esp_err_t spi_device_acquire_bus(void *spi,unsigned timeout){assert(spi&&timeout==portMAX_DELAY);acquired++;return acquire_error;}
static void spi_device_release_bus(void *spi){assert(spi);released++;}
static esp_err_t spi_device_polling_transmit(void *spi,spi_transaction_t *t)
{assert(spi&&t->length==expected_size*8&&t->tx_buffer!=expected);assert(!memcmp(t->tx_buffer,expected,expected_size));
 assert((uintptr_t)t->tx_buffer%4==0&&(uintptr_t)t->rx_buffer%4==0);sent++;
 if(t->rx_buffer)memset(t->rx_buffer,0x5a,expected_size);
 return transfer_error;}
''' + source + r'''
int main(void) {
 himax_device_t d={.spi=&d,.have_reset=true,.ready=true,.transfer_max=4096};
 solar_os_sscma_spi_t r={0};assert(!maintenance_control(&d,&r)&&!allocated&&!sent);
 d.have_reset=false;assert(maintenance_control(&d,&r)==ESP_ERR_NOT_SUPPORTED);d.have_reset=true;
 d.transfer_max=256;assert(maintenance_control(&d,&r)==ESP_ERR_NOT_SUPPORTED);d.transfer_max=4096;
 uint8_t tx[263],rx[263];memset(tx,0xa5,sizeof(tx));memset(rx,0,sizeof(rx));
 expected=tx;expected_size=262;r.tx=tx;r.rx=rx;r.length=263;
 assert(maintenance_control(&d,&r)==ESP_ERR_INVALID_ARG&&!allocated);r.length=262;
 d.started=true;assert(maintenance_control(&d,&r)==ESP_ERR_INVALID_STATE);d.started=false;
 deny=true;assert(maintenance_control(&d,&r)==ESP_ERR_NO_MEM);deny=false;
 assert(!maintenance_control(&d,&r)&&allocated==1&&sent==1&&acquired==released&&!d.ready&&rx[261]==0x5a);
 memset(rx,0,sizeof(rx));transfer_error=ESP_FAIL;
 assert(maintenance_control(&d,&r)==ESP_FAIL&&allocated==1&&rx[0]==0&&acquired==released);
 acquire_error=ESP_ERR_TIMEOUT;unsigned before=released;
 assert(maintenance_control(&d,&r)==ESP_ERR_TIMEOUT&&released==before);
 r=(solar_os_sscma_spi_t){.reset=true};assert(!maintenance_control(&d,&r));
 assert(invalidated==1&&prepared==1&&!d.maintenance&&d.ready);
}
''', "-I", str(ROOT / "src/services"), "-I", str(ROOT / "include"))

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
            service = Path(tmp) / "native-media.o"
            handles = Path(tmp) / "native-handles.o"
            abi_flags = ["cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-pthread",
                         "-fsanitize=address,undefined", "-fno-pie",
                         "-DSOLAR_OS_PACKAGE_SERVICE_MEDIA_SAMPLE=1", "-DSOLAR_OS_PACKAGE_SERVICE_PIPELINE=1",
                         "-I" + str(ROOT / "tests/host/native_abi_stubs"), "-I" + str(ROOT / "tests/host"),
                         "-I" + str(ROOT / "src"), "-I" + str(ROOT / "src/services"), "-I" + str(ROOT / "include")]
            for source, output in (("solar_os_native_media.c", service), ("solar_os_native_handles.c", handles)):
                subprocess.run([*abi_flags, "-c", str(ROOT / "src/services" / source), "-o", str(output)], check=True)
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
                    str(ROOT / "tests/host/native_memory_test_support.c"), str(service), str(handles),
                    str(ROOT / "modules/himax/provider.c"),
                    str(ROOT / "modules/himax/manager.c"),
                    str(ROOT / "src/services/solar_os_processor.c"),
                    str(ROOT / "src/services/solar_os_pipeline.c"),
                    str(ROOT / "src/services/solar_os_media_sample.c"),
                    str(ROOT / "src/services/solar_os_json_scan.c"),
                    str(ROOT / "src/drivers/himax_hx6538.c"),
                    str(cjson / "cJSON.c"), "-lm", "-lcrypto", "-o", str(binary),
                ], check=True)
                subprocess.run([str(binary)], check=True, timeout=60)
