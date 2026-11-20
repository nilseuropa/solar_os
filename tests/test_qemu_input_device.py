"""Exercise the real QEMU input device through QMP and physical MMIO.

Set SOLAR_OS_QEMU_INPUT_TEST_BINARY to the extended qemu-system-xtensa binary.
No firmware image or graphical desktop is required.
"""

import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time
import unittest


BINARY = os.environ.get("SOLAR_OS_QEMU_INPUT_TEST_BINARY")
ROOT = Path(__file__).resolve().parents[1]
REG = {name: int(value, 0) for name, value in re.findall(
    r"#define SOLAR_OS_QEMU_INPUT_(\w+) (0x[0-9a-f]+|[0-9]+)U",
    (ROOT / "include/solar_os_qemu_input_protocol.h").read_text())}


@unittest.skipUnless(BINARY, "set SOLAR_OS_QEMU_INPUT_TEST_BINARY to run emulator tests")
class QemuInputDeviceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temporary.name)
        cls.log = (cls.directory / "qemu.log").open("w+b")
        cls.process = subprocess.Popen([
            BINARY, "-machine", "esp32s3", "-display", "none", "-serial", "null",
            "-monitor", "none", "-nic", "none",
            "-qmp", f"unix:{cls.directory}/qmp,server=on,wait=off",
            "-qtest", f"unix:{cls.directory}/qtest,server=on,wait=off",
        ], stdout=cls.log, stderr=subprocess.STDOUT)
        cls.addClassCleanup(cls.cleanup)
        for name in ("qmp", "qtest"):
            connection = socket.socket(socket.AF_UNIX)
            connection.settimeout(5)
            deadline = time.monotonic() + 10
            while True:
                try:
                    connection.connect(str(cls.directory / name))
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    if time.monotonic() > deadline or cls.process.poll() is not None:
                        cls.log.seek(0)
                        raise RuntimeError(cls.log.read().decode(errors="replace"))
                    time.sleep(.05)
            setattr(cls, name, connection.makefile("rwb", buffering=0))
            cls.addClassCleanup(connection.close)
        json.loads(cls.qmp.readline())
        cls.command("qmp_capabilities")

    @classmethod
    def cleanup(cls):
        for name in ("qmp", "qtest"):
            if hasattr(cls, name): getattr(cls, name).close()
        if cls.process.poll() is None: cls.process.terminate()
        cls.process.wait(timeout=5)
        cls.log.close()
        cls.temporary.cleanup()

    @classmethod
    def command(cls, name, arguments=None):
        cls.qmp.write((json.dumps({"execute": name, "arguments": arguments or {}}) + "\n").encode())
        while True:
            response = json.loads(cls.qmp.readline())
            if "error" in response: raise AssertionError(response)
            if "return" in response: return response["return"]

    def mmio(self, register, value=None):
        address = REG["BASE"] + REG["REG_" + register]
        request = f"readl {address:#x}" if value is None else f"writel {address:#x} {value:#x}"
        self.qtest.write((request + "\n").encode())
        response = self.qtest.readline().decode().split()
        self.assertEqual(response[0], "OK", response)
        return int(response[1], 0) if value is None else None

    def event(self):
        result = tuple(self.mmio(field) for field in ("TYPE", "A", "B", "C", "D"))
        self.mmio("POP", 1)
        return result

    def focus(self, focused):
        self.command("qom-set", {"path": "/machine/soc/solar-input", "property": "focused", "value": focused})

    def setUp(self):
        self.focus(True)
        self.mmio("RESET", 1)
        self.assertEqual(self.event()[0], REG["RELEASE_ALL"])
        mice = self.command("query-mice")
        touch = next(m for m in mice if m["name"] == "SolarOS touch")
        self.command("human-monitor-command", {"command-line": f'mouse_set {touch["index"]}'})

    @staticmethod
    def key(code, down):
        return {"type": "key", "data": {"key": {"type": "qcode", "data": code}, "down": down}}

    def send(self, *events):
        self.command("input-send-event", {"events": list(events)})

    def test_hid_usages_modifiers_and_releases(self):
        self.assertEqual(self.mmio("MAGIC"), REG["MAGIC"])
        self.assertEqual(self.mmio("VERSION"), REG["VERSION"])
        self.send(self.key("shift", True), self.key("a", True), self.key("a", True),
                  self.key("a", False), self.key("shift", False))
        self.assertEqual(self.mmio("COUNT"), 4)
        self.assertEqual(self.event()[:4], (REG["KEY"], 0xe1, 2, 1))
        self.assertEqual(self.event()[:4], (REG["KEY"], 4, 2, 1))
        self.assertEqual(self.event()[:4], (REG["KEY"], 4, 2, 0))
        self.assertEqual(self.event()[:4], (REG["KEY"], 0xe1, 0, 0))

    def test_absolute_touch_and_wheel(self):
        self.send({"type": "abs", "data": {"axis": "x", "value": 16384}},
                  {"type": "abs", "data": {"axis": "y", "value": 32767}},
                  {"type": "btn", "data": {"button": "left", "down": True}})
        self.assertEqual(self.event(), (REG["TOUCH"], 16384, 32767, 1, 0))
        self.send({"type": "btn", "data": {"button": "left", "down": False}},
                  {"type": "btn", "data": {"button": "wheel-up", "down": True}})
        self.assertEqual(self.event(), (REG["TOUCH"], 16384, 32767, 0, 1))

    def test_relative_mouse(self):
        mouse = next(m for m in self.command("query-mice") if m["name"] == "SolarOS mouse")
        self.command("human-monitor-command", {"command-line": f'mouse_set {mouse["index"]}'})
        self.send({"type": "rel", "data": {"axis": "x", "value": 7}},
                  {"type": "rel", "data": {"axis": "y", "value": -9}},
                  {"type": "btn", "data": {"button": "right", "down": True}})
        self.assertEqual(self.event(), (REG["MOUSE"], 7, 0xfffffff7, 2, 0))

    def test_focus_loss_does_not_invalidate_a_latched_record(self):
        self.send(self.key("a", True))
        self.assertEqual(self.mmio("TYPE"), REG["KEY"])
        generation = self.mmio("GENERATION")
        self.focus(False)
        self.assertNotEqual(self.mmio("GENERATION"), generation)
        self.assertEqual(self.mmio("A"), 4)  # Existing read finishes consistently.
        self.mmio("POP", 1)
        self.assertEqual(self.event()[0], REG["RELEASE_ALL"])
        self.send(self.key("b", True))
        self.assertEqual(self.mmio("COUNT"), 0)

    def test_overflow_inserts_a_neutral_boundary(self):
        events = [self.key("a", bool(i % 2 == 0)) for i in range(400)]
        self.send(*events)
        self.assertGreater(self.mmio("DROPPED"), 0)
        self.assertEqual(self.event()[0], REG["RELEASE_ALL"])


if __name__ == "__main__":
    unittest.main()
