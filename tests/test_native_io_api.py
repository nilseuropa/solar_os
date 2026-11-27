from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions
ROOT = Path(__file__).resolve().parents[1]


class NativeIoApiTest(unittest.TestCase):
    def test_resource_gpio_adc_and_pwm_values_and_cleanup(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, 'gpio_owned_write.inc').write_text(production_functions(
                (ROOT / 'src/services/solar_os_gpio.c').read_text(), ('gpio_resource_owner', 'gpio_claim_is_ours',
                    'gpio_claim_direct', 'solar_os_gpio_read', 'solar_os_gpio_write', 'solar_os_gpio_write_owned',
                    'solar_os_gpio_mode_name', 'solar_os_gpio_pull_name', 'solar_os_gpio_parse_mode', 'solar_os_gpio_parse_pull')))
            Path(tmp, 'pin_labels.inc').write_text(production_functions(
                (ROOT / 'src/services/solar_os_pins.c').read_text(),
                ('solar_os_pin_policy_name', 'solar_os_connector_pin_kind_name')))
            binary = Path(tmp) / 'io'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-I', tmp,
                '-DSOLAR_OS_PACKAGE_SERVICE_RESOURCES=1', '-DSOLAR_OS_PACKAGE_SERVICE_GPIO=1',
                '-DSOLAR_OS_PACKAGE_SERVICE_EXPANSION=1',
                '-DSOLAR_OS_PACKAGE_SERVICE_ADC=1', '-DSOLAR_OS_PACKAGE_SERVICE_PWM=1', '-DSOLAR_OS_BOARD_HAS_GPIO=1',
                '-DESP_ERR_NOT_ALLOWED=0x10d', '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', str(ROOT / 'tests/host/native_abi_stubs'), '-I', str(ROOT / 'tests/host/gpio_controller_stubs'),
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'src/drivers'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_io_api_test.c'),
                *(str(ROOT / ('src/services/' + name)) for name in ['solar_os_resources.c', 'solar_os_pwm.c',
                    'solar_os_native_handles.c', 'solar_os_native_io.c', 'solar_os_gpio_controller.c',
                    'solar_os_native_device_providers.c']), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_disabled_and_adc_only_builds_do_not_pull_optional_backends(self):
        for defines in ([], ['-DSOLAR_OS_PACKAGE_SERVICE_ADC=1']):
            with self.subTest(defines=defines), tempfile.TemporaryDirectory() as tmp:
                source = '#include <stddef.h>\n#include "solar_os_native_io.h"\n'
                if defines:
                    source += '#include "solar_os_adc.h"\nsize_t solar_os_adc_pin_count(void){return 0;}\n'
                    source += 'bool solar_os_adc_get_pin_info(size_t i,solar_os_adc_pin_info_t *p){(void)i;(void)p;return false;}\n'
                    source += 'esp_err_t solar_os_adc_read(int i,solar_os_adc_sample_t *p){(void)i;(void)p;return ESP_ERR_NOT_SUPPORTED;}\n'
                source += 'int main(void){return solar_os_native_io_get_service("missing",1,8)!=NULL;}\n'
                binary = Path(tmp) / 'disabled'
                subprocess.run(['cc', '-x', 'c', '-std=c11', '-Wall', '-Wextra', '-Werror', *defines,
                    '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'),
                    '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'), '-',
                    str(ROOT / 'src/services/solar_os_native_io.c'), '-o', str(binary)], input=source, text=True, check=True)
                subprocess.run([str(binary)], check=True)
