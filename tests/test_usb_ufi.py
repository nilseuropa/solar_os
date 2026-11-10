from pathlib import Path
import importlib.util
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class UsbUfiTest(unittest.TestCase):
    def test_cbi_protocol_and_media_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "ufi"
            subprocess.run(["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/drivers"),
                            str(ROOT / "src/drivers/usb_ufi.c"), str(ROOT / "tests/host/usb_ufi_test.c"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_pinned_msc_patch_and_drift_rejection(self):
        spec = importlib.util.spec_from_file_location("patch_usb_msc", ROOT / "scripts/patch_usb_msc.py")
        patch = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(patch)
        component = ROOT / "managed_components/espressif__usb_host_msc"
        if not component.is_dir():
            self.skipTest("Pinned managed MSC component unavailable")
        for name in patch.HASHES:
            source = (component / "src" / name).read_text()
            generated = patch.patched_source(name, source)
            self.assertIn("solar_os_usb_ufi_msc_", generated)
            with self.assertRaises(ValueError):
                patch.patched_source(name, source + "\n")

    def test_sdk_control_interrupt_lengths_and_transfer_ownership(self):
        spec = importlib.util.spec_from_file_location("patch_usb_msc", ROOT / "scripts/patch_usb_msc.py")
        patch = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(patch)
        source = ROOT / "managed_components/espressif__usb_host_msc/src/msc_host.c"
        if not source.is_file():
            self.skipTest("Pinned managed MSC component unavailable")
        generated = patch.patched_source("msc_host.c", source.read_text())
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / "msc_endpoint.inc").write_text(production_functions(generated,
                ("solar_os_usb_ufi_msc_endpoint", "msc_bulk_transfer")))
            (stage / "msc_adsc.inc").write_text(production_functions(
                (ROOT / "src/drivers/usb_ufi_msc.c").read_text(), ("adsc",)))
            (stage / "msc_wait.inc").write_text(production_functions(generated,
                ("wait_for_transfer_done",)).replace("wait_for_transfer_done(", "test_wait_for_transfer_done("))
            binary = stage / "transfers"
            subprocess.run(["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            "-I", str(stage), "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/drivers"),
                            str(ROOT / "tests/host/usb_msc_transfer_test.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
