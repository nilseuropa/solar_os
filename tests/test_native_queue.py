from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeQueueTest(unittest.TestCase):
    def test_owned_fifo_copy_and_inflight_close(self):
        host = ROOT / "tests/host"
        services = ROOT / "src/services"
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / "freertos").mkdir()
            (folder / "freertos/queue.h").write_text('''
#pragma once
#include "freertos/FreeRTOS.h"
typedef struct test_queue *QueueHandle_t;
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
BaseType_t xQueuePeek(QueueHandle_t, void *, TickType_t);
BaseType_t xQueueOverwrite(QueueHandle_t, const void *);
BaseType_t xQueueReset(QueueHandle_t);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t);
''')
            binary = folder / "queue"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-fsanitize=address,undefined", "-no-pie", "-DconfigTICK_RATE_HZ=100",
                "-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1", f"-I{folder}", f"-I{host / 'hid_stubs'}",
                f"-I{host}", f"-I{ROOT / 'include'}", f"-I{ROOT / 'src'}", f"-I{services}",
                str(services / "solar_os_native_queue.c"), str(services / "solar_os_native_handles.c"),
                str(host / "native_queue_test.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
