"""Test the actual host PTY against QEMU's UART0 without running firmware.

Set SOLAR_OS_QEMU_TEST_BINARY to the pinned qemu-system-xtensa executable.
The extended input-test executable can also be used.
"""

import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import solaros_qemu as qemu

BINARY = os.environ.get("SOLAR_OS_QEMU_TEST_BINARY") or os.environ.get("SOLAR_OS_QEMU_INPUT_TEST_BINARY")


@unittest.skipUnless(BINARY and os.name == "posix", "set SOLAR_OS_QEMU_TEST_BINARY on a POSIX host")
class QemuSerialTests(unittest.TestCase):
    def setUp(self):
        import tty

        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        image = self.directory / "flash.bin"
        with image.open("wb") as stream: stream.truncate(qemu.FLASH_BYTES)
        log = (self.directory / "qemu.log").open("w+b")
        self.addCleanup(log.close)
        command = qemu.qemu_command(Path(BINARY), "qemu_esp32s3", image, uart0_mode="pty")
        command += ["-S", "-qtest", f"unix:{self.directory}/qtest,server=on,wait=off"]
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
        self.addCleanup(self.stop)
        connection = socket.socket(socket.AF_UNIX)
        self.addCleanup(connection.close)
        connection.settimeout(5)
        deadline = time.monotonic() + 10
        while True:
            try:
                connection.connect(str(self.directory / "qtest"))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() > deadline or self.process.poll() is not None:
                    log.seek(0)
                    self.fail(log.read().decode(errors="replace"))
                time.sleep(.05)
        self.qtest = connection.makefile("rwb", buffering=0)
        self.addCleanup(self.qtest.close)
        self.port = self.directory / "uart0"
        self.assertTrue(self.port.is_symlink())
        self.serial = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.addCleanup(os.close, self.serial)
        tty.setraw(self.serial)
        # QEMU checks for a newly opened PTY slave on its host timer. Wait for
        # a received byte before transmitting; an unopened UART drops output.
        os.write(self.serial, b"~")
        deadline = time.monotonic() + 5
        while not (self.mmio(0x1c) & 0xff):
            self.assertLess(time.monotonic(), deadline, "UART0 did not connect to the PTY")
            time.sleep(.01)
        self.assertEqual(self.mmio(0) & 0xff, ord("~"))

    def stop(self):
        if self.process.poll() is None: self.process.terminate()
        self.process.wait(timeout=5)
        self.process.stdin.close()

    def mmio(self, offset, value=None):
        address = 0x60000000 + offset
        request = f"readl {address:#x}" if value is None else f"writel {address:#x} {value:#x}"
        self.qtest.write((request + "\n").encode())
        response = self.qtest.readline().decode().split()
        self.assertEqual(response[0], "OK", response)
        return int(response[1], 0) if value is None else None

    def read_serial(self, count):
        result = b""
        deadline = time.monotonic() + 5
        while len(result) < count and time.monotonic() < deadline:
            if select.select([self.serial], [], [], .1)[0]:
                result += os.read(self.serial, count - len(result))
        self.assertEqual(len(result), count, result)
        return result

    def test_uart0_transfers_binary_and_control_bytes_in_both_directions(self):
        payload = b"\x00\x01c\x03\x04\x11\x13\x1b[6n\r\n\x80\xff"
        for byte in payload: self.mmio(0, byte)
        self.assertEqual(self.read_serial(len(payload)), payload)
        os.write(self.serial, payload)
        received = bytearray()
        deadline = time.monotonic() + 5
        while len(received) < len(payload) and time.monotonic() < deadline:
            if self.mmio(0x1c) & 0xff:
                received.append(self.mmio(0) & 0xff)
            else:
                time.sleep(.01)
        self.assertEqual(bytes(received), payload)
        self.assertIsNone(self.process.poll(), "UART control bytes must not control QEMU")
        self.assertEqual((self.directory / "uart0.log").read_bytes(), payload)

    def test_monitor_quit_is_separate_and_removes_the_virtual_port(self):
        self.process.stdin.write(b"quit\n")
        self.process.stdin.flush()
        self.assertEqual(self.process.wait(timeout=5), 0)
        self.assertFalse(self.port.is_symlink())


if __name__ == "__main__":
    unittest.main()
