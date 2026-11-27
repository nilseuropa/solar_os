from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SystemApiCoreTest(unittest.TestCase):
    def test_real_service_host_links_and_runs_without_an_elf_loader(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / 'freertos').mkdir()
            (stage / 'freertos/FreeRTOS.h').write_text(
                '#pragma once\n#include <stdint.h>\n'
                f'#include "{ROOT}/tests/host/native_abi_stubs/freertos/FreeRTOS.h"\n'
                'typedef uint32_t TickType_t;\n#define configTICK_RATE_HZ 1000U\n#define portMAX_DELAY UINT32_MAX\n')
            (stage / 'freertos/task.h').write_text('''#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
typedef uint32_t StackType_t;
typedef struct { unsigned unused; } StaticTask_t;
enum { eSuspended };
static inline void vTaskDelay(TickType_t n) { (void)n; }
static inline void vTaskSuspend(TaskHandle_t h) { (void)h; }
static inline void vTaskDelete(TaskHandle_t h) { (void)h; }
static inline unsigned uxTaskPriorityGet(TaskHandle_t h) { (void)h; return 1; }
static inline int eTaskGetState(TaskHandle_t h) { (void)h; return eSuspended; }
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)1; }
static inline TaskHandle_t xTaskCreateStaticPinnedToCore(void (*entry)(void *), const char *name,
    uint32_t size, void *arg, unsigned priority, StackType_t *stack, StaticTask_t *task, uint32_t core)
{ (void)entry; (void)name; (void)size; (void)arg; (void)priority; (void)stack; (void)task; (void)core; return 0; }
''')
            (stage / 'sdkconfig.h').write_text('#define CONFIG_IDF_TARGET "host"\n#define CONFIG_FREERTOS_NUMBER_OF_CORES 2\n')
            (stage / 'solar_os_config.h').write_text('#define SOLAR_OS_PACKAGE_CORE_RUNTIME 1\n'
                '#define SOLAR_OS_BUILTIN_COMPONENTS(X) \\\n'
                'X(SOLAR_OS_MODULE_TYPE_LIBRARY, "core-test", "Test library", "1.0.0", 1, solar_test_library_entry, 0, NULL, NULL) \\\n'
                'X(SOLAR_OS_MODULE_TYPE_JOB, "core-job", "Test job", "1.0.0", 1, solar_test_job_entry, 0, NULL, NULL) \\\n'
                'X(SOLAR_OS_MODULE_TYPE_APP, "core-ui", "Test app", "1.0.0", 2, solar_test_app_entry, 0, NULL, &solar_test_app_manifest)\n'
                '#define SOLAR_OS_BUILTIN_APP_MANIFESTS(X) X(solar_test_app_manifest)\n')
            providers = tomllib.loads((ROOT / 'api/native/services.toml').read_text())['provider']
            stubs = []
            for provider in providers:
                if provider['parent'] == 'root':
                    if provider['getter'] == 'solar_os_native_components_get_service':
                        continue
                    stubs.append('const void *' + provider['getter'] + '(const char *name, uint32_t abi, uint32_t size)'
                                 '{ (void)name; (void)abi; (void)size; return NULL; }\n')
            (stage / 'system_core_providers.inc').write_text(''.join(stubs))
            binary = stage / 'core'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-I', str(stage), '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'),
                '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/system_api_core_test.c'), str(ROOT / 'src/services/solar_os_system_api.c'),
                str(ROOT / 'src/services/solar_os_native_runtime.c'),
                str(ROOT / 'src/services/solar_os_components.c'), str(ROOT / 'src/services/solar_os_native_components.c'),
                '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
            undefined = subprocess.check_output(['nm', '-u', str(binary)], text=True)
            self.assertNotIn('esp_elf', undefined)
