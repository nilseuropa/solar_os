from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class NativeMqttTest(unittest.TestCase):
    def test_shared_broker_binary_events_and_concurrent_disconnect(self):
        host = ROOT / "tests/host"
        services = ROOT / "src/services"
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            (stage / "freertos").mkdir()
            support = {
                "esp_crt_bundle.h": '#include "esp_err.h"\nesp_err_t esp_crt_bundle_attach(void *);',
                "esp_random.h": '#include <stdint.h>\nuint32_t esp_random(void);',
                "freertos/queue.h": (
                    f'#include "{host / "hid_stubs/freertos/queue.h"}"\n'
                    'UBaseType_t uxQueueMessagesWaiting(QueueHandle_t);\n'),
                "freertos/semphr.h": (
                    '#include "freertos/FreeRTOS.h"\n#undef portMAX_DELAY\n'
                    f'#include "{host / "native_abi_stubs/freertos/semphr.h"}"\n'
                    'static inline void vSemaphoreDelete(SemaphoreHandle_t sem) {\n'
                    '    pthread_mutex_destroy(sem); free(sem);\n}\n'),
                "solar_os_task.h": '''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#define SOLAR_OS_TASK_ROLE_BACKGROUND 1
typedef struct { bool admitted; } solar_os_task_managed_admission_t;
bool solar_os_task_admit_managed(const char *, size_t, int, bool, solar_os_task_managed_admission_t *);
void solar_os_task_note_managed_result(const char *, size_t, int,
    solar_os_task_managed_admission_t *, bool);
''',
            }
            for name, contents in support.items():
                (stage / name).write_text(contents)
            flags = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{stage}", f"-I{host / 'mqtt_stubs'}",
                     f"-I{host / 'hid_stubs'}", f"-I{host}", f"-I{ROOT / 'include'}", f"-I{ROOT / 'src'}",
                     f"-I{services}"]
            subprocess.run([*flags, "-DSOLAR_OS_PACKAGE_SERVICE_MQTT=0", "-fsyntax-only",
                            str(services / "solar_os_native_mqtt.c")], check=True)
            binary = stage / "mqtt"
            subprocess.run([*flags, "-pthread", "-fsanitize=address,undefined", "-no-pie",
                            "-DSOLAR_OS_PACKAGE_SERVICE_MQTT=1", "-include", str(host / "compat.h"),
                            str(services / "solar_os_mqtt.c"), str(services / "solar_os_native_mqtt.c"),
                            str(host / "native_mqtt_test.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
