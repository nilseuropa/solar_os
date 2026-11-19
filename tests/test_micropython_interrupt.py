from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MicroPythonInterruptTest(unittest.TestCase):
    def test_cleanup_grace_pending_delivery_and_new_interpreter(self):
        headers = {
            "esp_log.h": '#define ESP_LOGI(tag, ...) ((void)(tag))\n',
            "esp_random.h": 'static inline unsigned esp_random(void) { return 0; }\n',
            "freertos/FreeRTOS.h": (
                '#include <stdint.h>\ntypedef uint32_t TickType_t;\n'
                '#define pdMS_TO_TICKS(ms) (ms)\n'
            ),
            "freertos/task.h": (
                'uint32_t xTaskGetTickCount(void);\n'
                'void vTaskDelay(uint32_t ticks);\n'
            ),
            "py/mphal.h": '',
            "py/runtime.h": (
                '#include <stdbool.h>\n#include <stddef.h>\n'
                'struct test_vm_state { void *mp_pending_exception; };\n'
                'extern struct test_vm_state test_vm;\n'
                '#define MP_STATE_THREAD(field) (test_vm.field)\n'
                '#define MP_OBJ_NULL NULL\n'
                'void mp_sched_keyboard_interrupt(void);\n'
                'void mp_handle_pending(bool raise);\n'
            ),
        }
        with tempfile.TemporaryDirectory() as tmp:
            for name, content in headers.items():
                path = Path(tmp, name)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            binary = Path(tmp, "interrupt")
            subprocess.run([
                "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                "-I", tmp,
                str(ROOT / "components/micropython_embed/overrides/port/mphalport.c"),
                str(ROOT / "tests/host/micropython_interrupt_test.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
