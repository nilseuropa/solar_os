"""Exercise Link queue lifetime and the production stream over a lossy transport."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LinkStreamTest(unittest.TestCase):
    def test_loss_reconnect_backpressure_and_close_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "freertos").mkdir()
            (path / "freertos/FreeRTOS.h").write_text(
                '#pragma once\n#include <stdint.h>\n'
                'typedef unsigned UBaseType_t;\ntypedef uint32_t TickType_t;\n'
                '#define pdTRUE 1\n#define pdFALSE 0\n'
                '#define pdMS_TO_TICKS(x) (x)\n#define pdTICKS_TO_MS(x) (x)\n'
                '#define portMAX_DELAY UINT32_MAX\n'
                'typedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n'
                '#define portENTER_CRITICAL(lock) ((void)(lock))\n'
                '#define portEXIT_CRITICAL(lock) ((void)(lock))\n')
            (path / "freertos/semphr.h").write_text(
                '#pragma once\ntypedef int *SemaphoreHandle_t;\ntypedef int StaticSemaphore_t;\n'
                'static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s)'
                '{return s;}\n'
                'static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {static int m;return &m;}\n'
                'static inline int xSemaphoreTake(SemaphoreHandle_t s,unsigned t) {(void)s;(void)t;return 1;}\n'
                'static inline int xSemaphoreGive(SemaphoreHandle_t s) {(void)s;return 1;}\n')
            (path / "freertos/task.h").write_text(
                '#pragma once\n#include "FreeRTOS.h"\n'
                'TickType_t xTaskGetTickCount(void);\nvoid vTaskDelay(TickType_t);\n')
            (path / "freertos/queue.h").write_text(
                '#pragma once\n#include <stddef.h>\n'
                'typedef struct test_queue *QueueHandle_t;\n'
                'int xQueueSend(QueueHandle_t,const void *,unsigned);\n'
                'int xQueueReceive(QueueHandle_t,void *,unsigned);\n'
                'unsigned uxQueueSpacesAvailable(QueueHandle_t);\n'
                'unsigned uxQueueMessagesWaiting(QueueHandle_t);\n'
                'void xQueueReset(QueueHandle_t);\n')
            (path / "esp_random.h").write_text('#pragma once\n#include <stdint.h>\nuint32_t esp_random(void);\n')
            (path / "esp_mac.h").write_text(
                '#pragma once\n#include "esp_err.h"\n#include <stdint.h>\n'
                'static inline esp_err_t esp_efuse_mac_get_default(uint8_t *mac) '
                '{ (void)mac; return ESP_FAIL; }\n')
            flags = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                     '-DESP_ERR_INVALID_VERSION=0x10a', '-DSOLAR_OS_PACKAGE_SERVICE_LINK=1', '-DSOLAR_OS_PACKAGE_SERVICE_MESHCORE=1', '-Wall', '-Wextra', '-Werror',
                     '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                     '-include', str(ROOT / 'tests/host/compat.h'),
                     '-I' + directory, '-I' + str(ROOT / 'tests/host'), '-I' + str(ROOT / 'include'),
                     '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'src/services')]
            cases = {
                'stream': [ROOT / 'tests/host/link_stream_test.c'],
                'close-and-abi': [
                    ROOT / 'tests/host/link_close_test.c',
                    ROOT / 'src/services/solar_os_link.c',
                    ROOT / 'src/services/solar_os_link_stream.c',
                    ROOT / 'src/services/solar_os_native_link.c',
                    ROOT / 'src/services/solar_os_link_repeater.c',
                    ROOT / 'src/services/solar_os_meshcore_stream.c',
                    ROOT / 'src/services/solar_os_meshcore_stream_codec.c',
                    ROOT / 'src/services/solar_os_native_handles.c',
                ],
            }
            for name, sources in cases.items():
                with self.subTest(name=name):
                    binary = path / name
                    subprocess.run([*flags, '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                                    *map(str, sources), '-o', str(binary)], check=True)
                    subprocess.run([str(binary)], check=True, timeout=30)



if __name__ == '__main__':
    unittest.main()
