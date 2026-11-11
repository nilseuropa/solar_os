from pathlib import Path
import importlib.util
import subprocess
import re
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class UsbSerialTest(unittest.TestCase):
    def test_hotplug_ownership_and_teardown(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory, "usb_serial")
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-D_GNU_SOURCE", "-pthread",
                "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer", "-include", str(ROOT / "tests/host/compat.h"),
                "-I" + str(ROOT / "tests/host/usb_serial_stubs"),
                "-I" + str(ROOT / "tests/host/registry_stubs"),
                "-I" + str(ROOT / "tests/host/usb_storage_stubs"),
                "-I" + str(ROOT / "tests/host"), "-I" + str(ROOT / "src/services"),
                "-I" + str(ROOT / "src/drivers"), "-I" + str(ROOT / "src"),
                str(ROOT / "tests/host/usb_serial_test.c"),
                str(ROOT / "src/services/solar_os_port.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    @staticmethod
    def compile_and_run(source):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(source)
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "tests/host"),
                            "-I" + str(ROOT / "src/services"),
                            "-I" + str(ROOT / "src/drivers"),
                            str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True, timeout=10)

    @staticmethod
    def function(source, name):
        match = re.search(r"^static [^;]*?\b" + name + r"\([^;]*?\)\s*\{", source, re.M)
        assert match, name
        opening = source.index("{", match.start())
        depth = 1
        end = opening + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"

    def test_composite_and_identical_device_discovery(self):
        source = (ROOT / "src/drivers/usb_serial_port.c").read_text()
        self.compile_and_run((ROOT / "tests/host/usb_serial_discovery_test.inc").read_text() +
                             self.function(source, "new_device") +
                             (ROOT / "tests/host/usb_serial_discovery_main.inc").read_text())

    def test_flash_transports_and_reset_sequences(self):
        source = (ROOT / "src/services/solar_os_flash.c").read_text()
        start = source.index("typedef struct {\n  esp_loader_port_t base;")
        end = source.index("} flash_loader_port_t;", start) + len("} flash_loader_port_t;")
        code = (ROOT / "tests/host/usb_flash_transport_test.inc").read_text() + source[start:end] + "\n"
        for name in ("flash_loader_port", "flash_port_flush_usb", "flash_port_init", "flash_port_deinit",
                     "flash_port_delay", "flash_port_start_timer", "flash_port_remaining", "flash_port_lines", "flash_port_usb_reset",
                     "flash_port_enter_bootloader", "flash_port_reset_target", "flash_port_change_rate",
                     "flash_port_write", "flash_port_read"):
            code += self.function(source, name)
        self.compile_and_run(code + (ROOT / "tests/host/usb_flash_transport_main.inc").read_text())

    def test_vcp_patch_rejects_dependency_drift(self):
        spec = importlib.util.spec_from_file_location("patch_usb_vcp", ROOT / "scripts/patch_usb_vcp.py")
        patch = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(patch)
        for kind in ("cp210x", "ch34x"):
            source = ROOT / f"managed_components/espressif__usb_host_{kind}_vcp/usb_host_{kind}_vcp.c"
            if not source.exists():
                self.skipTest("managed VCP source unavailable")
            text = source.read_text()
            generated = patch.patched_source(kind, text)
            self.assertTrue(generated.startswith(text))
            # The original VCP setup is preserved after an address-aware CDC open.
            addition = generated[len(text):]
            self.assertIn("cdc_acm_host_open(config, cdc_hdl_ret)", addition)
            self.assertIn(f"{kind}_line_coding_set", addition)
            if kind == "cp210x":
                self.assertIn("CP210X_CMD_IFC_ENABLE", addition)
            with self.assertRaises(ValueError):
                patch.patched_source(kind, text + "\n")


if __name__ == "__main__":
    unittest.main()
