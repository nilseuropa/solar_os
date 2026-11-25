from pathlib import Path
import subprocess
import tempfile
import unittest
from test_keyboard_layouts import write_keyboard_layouts

ROOT = Path(__file__).resolve().parents[1]


class NativeInputActionsTest(unittest.TestCase):
    def test_real_core_rules_routing_workers_generations_stop_and_emission(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            write_keyboard_layouts(directory)
            (stage / 'freertos').mkdir()
            (stage / 'freertos/FreeRTOS.h').write_text(
                f'#pragma once\n#include "{ROOT}/tests/host/registry_stubs/freertos/FreeRTOS.h"\n'
                'typedef int BaseType_t;\ntypedef unsigned UBaseType_t;\n#define pdFALSE 0\n#define pdPASS 1\n')
            (stage / 'freertos/task.h').write_text('''#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define tskNO_AFFINITY (-1)
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
''')
            (stage / 'freertos/queue.h').write_text('''#pragma once
#include "freertos/FreeRTOS.h"
typedef struct test_queue *QueueHandle_t;
BaseType_t xQueueSend(QueueHandle_t queue,const void *item,TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue,void *item,TickType_t wait);
BaseType_t xQueueReset(QueueHandle_t queue);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);
''')
            binary = stage / 'actions'
            sources = ['solar_os_input.c', 'solar_os_keys.c', 'solar_os_input_actions.c',
                'solar_os_input_action_match.c', 'solar_os_native_input_actions.c']
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1',
                '-Wall', '-Wextra', '-Werror', '-pthread', '-g', '-O1', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', str(stage), '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'),
                '-I', str(ROOT / 'src/services'), str(ROOT / 'tests/host/native_input_actions_test.c'),
                *[str(ROOT / 'src/services' / name) for name in sources], '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_unavailable_service_has_no_backend_dependency(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            source = stage / 'absent.c'
            source.write_text('#include <assert.h>\n#include "solar_os_native_input_actions.h"\n'
                'int main(void){assert(!solar_os_native_input_actions_get_service("input.actions",1,8));return 0;}\n')
            subprocess.run(['cc', '-std=c11', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=0', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(source), str(ROOT / 'src/services/solar_os_native_input_actions.c'), '-o', str(stage / 'absent')], check=True)
            subprocess.run([str(stage / 'absent')], check=True)
