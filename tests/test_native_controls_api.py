from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions
ROOT = Path(__file__).resolve().parents[1]


class NativeControlsApiTest(unittest.TestCase):
    def test_real_parameters_and_controls_through_stable_records_and_claims(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'controls'
            audio = (ROOT / 'src/services/solar_os_audio.c').read_text()
            state = re.search(r'typedef struct \{\n    bool registered;.*?\} audio_device_entry_t;', audio, re.S).group()
            functions = production_functions(audio, (
                'audio_device_id_valid', 'audio_register_input_controls',
                'solar_os_audio_register_device_ex', 'audio_unregister_device',
                'solar_os_audio_unregister_device', 'solar_os_audio_unregister_device_owned',
                'solar_os_audio_device_count', 'solar_os_audio_device_get',
                'solar_os_audio_device_get_info', 'solar_os_audio_output_available',
                'audio_mixer_enter', 'audio_mixer_leave', 'audio_mixer_select_output',
                'solar_os_audio_set_default_output', 'solar_os_audio_get_default_output',
                'audio_set_selected_output_volume', 'solar_os_audio_global_volume',
                'audio_apply_global_volume', 'solar_os_audio_set_volume', 'solar_os_audio_toggle_mute',
                'solar_os_audio_set_device_volume', 'audio_open_candidate', 'solar_os_audio_open_default',
                'audio_capture_args_valid', 'audio_capture_format_supported', 'solar_os_audio_capture_frames',
                'clamp_percent_u32', 'audio_abs_i16', 'solar_os_audio_measure_stream'))
            (Path(tmp) / 'audio_control_registry.inc').write_text(
                state + '\n#define AUDIO_LEVEL_BUFFER_SAMPLES 512\nstatic audio_device_entry_t audio_devices[SOLAR_OS_AUDIO_DEVICE_MAX];\n'
                'static portMUX_TYPE audio_devices_lock = portMUX_INITIALIZER_UNLOCKED;\n'
                'static char audio_default_output[SOLAR_OS_AUDIO_DEVICE_ID_MAX];\n'
                'static atomic_uchar audio_global_volume = 50;\n'
                'static bool audio_mixer_changing;\nstatic uint8_t audio_mute_restore_volume = 50;\n' + functions)
            display = (ROOT / 'src/services/solar_os_display.c').read_text()
            definitions = re.findall(r'^#define DISPLAY_BRIGHTNESS_PARAMETER_.*$', display, re.M)
            functions = production_functions(display, (
                'display_read_brightness', 'display_write_brightness',
                'display_parameter_get_brightness', 'display_parameter_set_brightness',
                'display_register_brightness', 'solar_os_display_get_brightness',
                'solar_os_display_set_brightness',
            ))
            (Path(tmp) / 'display_brightness_parameter.inc').write_text('\n'.join(definitions) + '\n' + functions)
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1', '-DSOLAR_OS_PACKAGE_SERVICE_CONTROLS=1',
                '-DSOLAR_OS_PACKAGE_SERVICE_AUDIO=1', '-DSOLAR_OS_PACKAGE_SERVICE_STREAMS=1',
                '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', tmp,
                '-I', str(ROOT / 'tests/host/native_abi_stubs'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_controls_api_test.c'),
                *(str(ROOT / ('src/services/' + name)) for name in ['solar_os_parameters.c', 'solar_os_controls.c',
                    'solar_os_controls_runtime.c', 'solar_os_native_handles.c', 'solar_os_native_controls_api.c',
                    'solar_os_native_audio_routes.c', 'solar_os_stream.c', 'solar_os_native_stream.c']), '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
