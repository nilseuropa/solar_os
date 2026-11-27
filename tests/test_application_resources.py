from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ApplicationResourcesTest(unittest.TestCase):
    def test_real_core_ownership_reverse_cleanup_retry_failure_and_stateless_apps(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / 'freertos').mkdir()
            (stage / 'freertos/FreeRTOS.h').write_text('#pragma once\n#include <stdint.h>\n'
                f'#include "{ROOT}/tests/host/native_abi_stubs/freertos/FreeRTOS.h"\n'
                'typedef uint32_t TickType_t;\n#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))\n')
            (stage / 'freertos/task.h').write_text('#pragma once\n#include "FreeRTOS.h"\nvoid vTaskDelay(TickType_t);\n')
            binary = stage / 'resources'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-g', '-O1', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                '-include', str(ROOT / 'tests/host/compat.h'), '-I', str(stage), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                '-I', str(ROOT / 'src/shell'), '-I', str(ROOT / 'components/u8g2/src/clib'),
                str(ROOT / 'tests/host/application_resources_test.c'), str(ROOT / 'src/solar_os.c'),
                str(ROOT / 'src/services/solar_os_native_handles.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
