"""Exercise the production Link v3 stream over a lossy two-ended transport."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LinkStreamTest(unittest.TestCase):
    def test_loss_reconnect_and_backpressure(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "freertos").mkdir()
            (path / "freertos/FreeRTOS.h").write_text(
                '#pragma once\n#include <stdint.h>\n'
                'typedef unsigned UBaseType_t;\ntypedef uint32_t TickType_t;\n'
                '#define pdTRUE 1\n#define pdFALSE 0\n'
                '#define pdMS_TO_TICKS(x) (x)\n#define pdTICKS_TO_MS(x) (x)\n'
                '#define portMAX_DELAY UINT32_MAX\n')
            (path / "freertos/semphr.h").write_text(
                '#pragma once\ntypedef int *SemaphoreHandle_t;\n'
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
            binary = path / "test"
            subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', '-include', str(ROOT / 'tests/host/compat.h'),
                '-I' + directory, '-I' + str(ROOT / 'tests/host'),
                '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/link_stream_test.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
