from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class AudioPDMTest(unittest.TestCase):
    def compile_and_run(self, name, production):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory, name)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-include", str(ROOT / "tests/host/compat.h"),
                "-I", str(ROOT / "tests/host/pdm_stubs"), "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src/drivers"), "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "src"), "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host" / (name + ".c")), str(ROOT / production),
                "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_lazy_dma_power_and_driver_cleanup(self):
        self.compile_and_run("audio_pdm_test", "src/drivers/audio_pdm.c")

    def test_stream_format_ownership_and_registration_rollback(self):
        self.compile_and_run("audio_pdm_service_test", "src/services/solar_os_audio_pdm.c")

    def test_sticky_microphone_binding_and_console(self):
        board = tomllib.loads((ROOT / "boards/manifests/seeed_reterminal_sticky.toml").read_text())
        mic = next(device for device in board["devices"] if device["name"] == "mic0")
        self.assertEqual(mic["driver"], "audio-pdm")
        self.assertEqual(mic["bindings"], {"i2s": 0, "clk": 19, "din": 20, "power": 38, "slot": 0})
        self.assertIn("audio_input", board["build"]["capabilities"])
        self.assertEqual(board["runtime"]["i2s_ports"], ["I2S_NUM_0"])
        self.assertIn(".pin = GPIO_NUM_38, .level = 0", board["defines"]["SOLAR_OS_BOARD_BOOT_OUTPUTS"])
        self.assertEqual(board["defines"]["SOLAR_OS_BOARD_PIN_UART_TX"], "GPIO_NUM_43")
        self.assertEqual(board["defines"]["SOLAR_OS_BOARD_PIN_UART_RX"], "GPIO_NUM_44")

    def test_microphone_level_uses_registered_capture_stream(self):
        source = (ROOT / "src/services/solar_os_audio.c").read_text()
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp / "audio_capture_level.inc").write_text(production_functions(source,
                ("clamp_percent_u32", "audio_abs_i16", "audio_measure_capture_stream")))
            binary = tmp / "level"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(tmp), "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src/services"),
                str(ROOT / "tests/host/audio_capture_level_test.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
