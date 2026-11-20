"""Exercise real host-directory operations through the QEMU MMIO device.

Set SOLAR_OS_QEMU_INPUT_TEST_BINARY to the extended qemu-system-xtensa binary.
"""
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "include/solar_os_qemu_hostfs_protocol.h").read_text()
ABI = {name: int(value, 0) for name, value in re.findall(
    r"#define SOLAR_OS_QEMU_HOSTFS_(\w+) (0x[0-9a-f]+|[0-9]+)U", HEADER)}
ERRORS = {name: index + 1 for index, name in enumerate(re.findall(r"X\((E\w+)\)", HEADER))}
BINARY = os.environ.get("SOLAR_OS_QEMU_INPUT_TEST_BINARY")


@unittest.skipUnless(BINARY, "set SOLAR_OS_QEMU_INPUT_TEST_BINARY for emulator tests")
class QemuHostFilesystemTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.shared = self.directory / "shared"
        self.shared.mkdir()
        self.process = None
        self.addCleanup(self.stop)
        self.start()

    def start(self):
        self.log = (self.directory / "qemu.log").open("wb")
        control = self.directory / "qtest"
        if control.exists(): control.unlink()
        self.process = subprocess.Popen([
            BINARY, "-machine", "esp32s3", "-display", "none", "-S",
            "-serial", "null", "-monitor", "none", "-nic", "none",
            "-global", f"solar-os-hostfs.root={self.shared}",
            "-qtest", f"unix:{control},server=on,wait=off",
        ], stdout=self.log, stderr=subprocess.STDOUT)
        self.connection = socket.socket(socket.AF_UNIX)
        self.connection.settimeout(5)
        deadline = time.monotonic() + 10
        while True:
            try:
                self.connection.connect(str(control))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() > deadline or self.process.poll() is not None:
                    self.fail((self.directory / "qemu.log").read_text())
                time.sleep(.02)
        self.qtest = self.connection.makefile("rwb", buffering=0)
        self.assertEqual(self.reg("MAGIC"), ABI["MAGIC"])
        self.assertEqual(self.reg("READY"), 1)

    def stop(self):
        if self.process is None: return
        if hasattr(self, "qtest"): self.qtest.close()
        self.connection.close()
        if self.process.poll() is None: self.process.terminate()
        self.process.wait(timeout=5)
        self.log.close()
        self.process = None

    def request(self, command):
        self.qtest.write((command + "\n").encode())
        result = self.qtest.readline().decode().split()
        self.assertEqual(result[0], "OK", result)
        return result[1] if len(result) > 1 else None

    def reg(self, name, value=None):
        address = ABI["BASE"] + ABI["REG_" + name]
        if value is None: return int(self.request(f"readl {address:#x}"), 0)
        self.request(f"writel {address:#x} {value & 0xffffffff:#x}")

    def data(self, value=None, count=40):
        address = ABI["BASE"] + ABI["REG_DATA"]
        if value is not None:
            self.request(f"write {address:#x} {len(value):#x} 0x{value.hex()}")
        else:
            return bytes.fromhex(self.request(f"read {address:#x} {count:#x}")[2:])

    def call(self, operation, *args, path=None, data=None, error=None):
        if path is not None: data = path.encode() + b"\0"
        if data is not None: self.data(data)
        for i in range(4):
            self.request(f'writel {ABI["BASE"] + ABI["REG_ARG0"] + i * 4:#x} {(args[i] if i < len(args) else 0) & 0xffffffff:#x}')
        self.reg("COMMAND", ABI[operation])
        result = struct.unpack("<i", struct.pack("<I", self.reg("RESULT")))[0]
        if error is not None:
            self.assertEqual(result, -1)
            self.assertIn(self.reg("ERROR"), [ERRORS[name] for name in ((error,) if isinstance(error, str) else error)])
        else:
            self.assertGreaterEqual(result, 0, (operation, self.reg("ERROR")))
        return result

    def open(self, name, flags=0): return self.call("OPEN", flags, 0o644, path=name)
    def read(self, fd, length):
        count = self.call("READ", fd, length)
        return self.data(count=count) if count else b""

    def test_read_write_seek_append_and_truncate(self):
        fd = self.open("binary", ABI["O_RDWR"] | ABI["O_CREATE"])
        payload = bytes(range(256)) * 16
        self.assertEqual(self.call("WRITE", fd, len(payload), data=payload), len(payload))
        self.call("FSYNC", fd)
        self.assertEqual((self.shared / "binary").read_bytes(), payload)
        self.call("SEEK", fd, 0, 0, 0)
        self.assertEqual(self.read(fd, len(payload)), payload)
        self.assertEqual(self.read(fd, 1), b"")
        self.call("TRUNCATE", fd, 3)
        self.call("FSTAT", fd)
        self.assertEqual(struct.unpack_from("<Q", self.data(), 8)[0], 3)
        self.call("CLOSE", fd)
        fd = self.open("binary", ABI["O_WRITE"] | ABI["O_APPEND"])
        self.call("WRITE", fd, 2, data=b"ok")
        self.call("CLOSE", fd)
        self.assertEqual((self.shared / "binary").read_bytes(), b"\0\1\2ok")

    def test_directories_metadata_rename_replace_and_remove(self):
        self.call("MKDIR", 0o755, path="sub")
        (self.shared / "sub/a.txt").write_text("first")
        (self.shared / "sub/b.txt").write_text("second")
        directory = self.call("OPENDIR", path="sub")
        self.assertEqual(self.call("READDIR", directory), 1)
        first = self.data(count=260)[4:].split(b"\0")[0]
        self.assertEqual(self.call("TELLDIR", directory), 1)
        self.call("SEEKDIR", directory, 0)
        self.call("READDIR", directory)
        self.assertEqual(self.data(count=260)[4:].split(b"\0")[0], first)
        self.call("CLOSEDIR", directory)
        self.call("RMDIR", path="sub", error="ENOTEMPTY")
        self.call("RENAME", data=b"sub/a.txt\0sub/b.txt\0")
        self.assertEqual((self.shared / "sub/b.txt").read_text(), "first")
        self.call("UTIME", 123, 0, 456, 0, path="sub/b.txt")
        self.call("STAT", path="sub/b.txt")
        self.assertEqual(struct.unpack_from("<Q", self.data(), 16)[0], 456)
        self.call("UNLINK", path="sub/b.txt")
        self.call("RMDIR", path="sub")
        self.call("STAT", path="sub", error="ENOENT")

    def test_selected_root_rejects_traversal_and_symlinks(self):
        outside = self.directory / "outside"
        outside.mkdir()
        (outside / "secret").write_text("keep")
        (self.shared / "link").symlink_to(outside / "secret")
        (self.shared / "dirlink").symlink_to(outside, target_is_directory=True)
        self.call("OPEN", path="../outside/secret", error="EACCES")
        self.call("OPEN", ABI["O_WRITE"] | ABI["O_TRUNCATE"], path="link", error="ELOOP")
        self.call("STAT", path="link", error="ELOOP")
        for operation in ("OPEN", "STAT", "MKDIR"):
            self.data(b"dirlink/secret\0")
            self.call(operation, error=("ENOTDIR", "ELOOP"))
        self.call("RENAME", data=b"link\0../outside/renamed\0", error="EACCES")
        self.assertEqual((outside / "secret").read_text(), "keep")

    def test_live_host_changes_and_restart_persistence(self):
        fd = self.open("saved", ABI["O_WRITE"] | ABI["O_CREATE"])
        self.call("WRITE", fd, 5, data=b"saved")
        self.call("FSYNC", fd)
        self.call("CLOSE", fd)
        self.stop()
        self.start()
        fd = self.open("saved")
        self.assertEqual(self.read(fd, 10), b"saved")
        self.call("CLOSE", fd)
        (self.shared / "saved").write_text("changed on host")
        fd = self.open("saved")
        self.assertEqual(self.read(fd, 30), b"changed on host")
        self.call("CLOSE", fd)

    def test_bounds_errors_and_reset_close_handles(self):
        self.open("new", ABI["O_CREATE"] | ABI["O_WRITE"])
        self.call("OPEN", ABI["O_CREATE"] | ABI["O_EXCLUSIVE"], path="new", error="EEXIST")
        self.call("OPEN", path="x" * ABI["PATH_SIZE"], error="ENAMETOOLONG")
        self.call("RESET")
        self.call("READ", 1, 1, error="EBADF")
        fd = self.open("new")
        self.call("READ", fd, ABI["DATA_SIZE"] + 1, error="EINVAL")
        self.call("USAGE")
        total, used, available = struct.unpack("<QQQ", self.data(count=24))
        self.assertGreater(total, 0)
        self.assertLessEqual(used, total)
        self.assertLessEqual(available, total)


if __name__ == "__main__": unittest.main()
