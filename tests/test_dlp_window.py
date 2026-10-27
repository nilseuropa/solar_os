"""Python sender against the actual C receiver, with faults on the byte stream."""
import ctypes
from dataclasses import replace
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from test_dlp_image import Clock
import dlp_image as dlp

ROOT = Path(__file__).resolve().parents[1]


class Port:
    def __init__(self, bridge, clock, fault=None):
        self.bridge, self.clock, self.fault = bridge, clock, fault
        self.tx, self.rx, self.wire = bytearray(), bytearray(), bytearray()
        self.requests = []
        self.held = None

    @staticmethod
    def packet(buffer):
        if len(buffer) < 32:
            return None
        length = 36 + struct.unpack_from("<H", buffer, 18)[0] if buffer[:4] == b"DLP1" else 256
        if len(buffer) < length:
            return None
        packet = bytes(buffer[:length])
        del buffer[:length]
        return packet

    def tick(self):
        self.clock.sleep(.001)
        self.bridge.bridge_tick(int(self.clock.now * 1_000_000))

    def write(self, data):
        n = min(11, len(data))
        self.tx.extend(data[:n])
        while (packet := self.packet(self.tx)) is not None:
            cell = dlp.decode_cell(packet)
            self.requests.append(cell)
            if cell.kind == dlp.PUSH:
                if cell.sequence == 3 and self.fault in ("loss", "corrupt"):
                    fault, self.fault = self.fault, None
                    if fault == "corrupt":
                        packet = packet[:-1] + bytes([packet[-1] ^ 1])
                    else:
                        continue
                if cell.sequence == 2 and self.fault == "reorder":
                    self.held = packet
                    continue
            self.bridge.bridge_feed(packet, len(packet))
            if cell.kind == dlp.PUSH and self.fault == "duplicate":
                self.bridge.bridge_feed(packet, len(packet))
            if cell.kind == dlp.PUSH and cell.sequence == 3 and self.held:
                self.bridge.bridge_feed(self.held, len(self.held))
                self.held, self.fault = None, None
        self.tick()
        return n

    def read(self, size):
        self.tick()
        data = ctypes.create_string_buffer(2048)
        n = self.bridge.bridge_read(data, len(data))
        self.wire.extend(data.raw[:n])
        while (packet := self.packet(self.wire)) is not None:
            cell = dlp.decode_cell(packet)
            if cell.opcode == dlp.WINDOW_SYNC and self.fault == "ack":
                self.fault = None
                continue
            if (cell.opcode == dlp.WINDOW_SYNC and self.fault == "completion"
                    and struct.unpack_from("<I", cell.payload, 20)[0]):
                self.fault = None
                continue
            self.rx.extend(packet)
        data = bytes(self.rx[:size])
        del self.rx[:size]
        return data


class WindowInteropTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        library = Path(cls.directory.name) / "receiver.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-I" + str(ROOT / "src/services"),
                        "-I" + str(ROOT / "protocol/display_link/include"),
                        str(ROOT / "tests/host/dlp_window_bridge.c"),
                        str(ROOT / "src/services/solar_os_dlp_receiver.c"),
                        str(ROOT / "protocol/display_link/src/sdlp.c"), "-o", str(library)], check=True)
        cls.bridge = ctypes.CDLL(str(library))
        cls.bridge.bridge_feed.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        cls.bridge.bridge_tick.argtypes = [ctypes.c_uint64]
        cls.bridge.bridge_read.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        cls.bridge.bridge_read.restype = ctypes.c_size_t

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def setUp(self):
        self.clock = Clock()
        for name in ("monotonic", "sleep"):
            patcher = patch.object(dlp.time, name, getattr(self.clock, name))
            patcher.start()
            self.addCleanup(patcher.stop)

    def client(self, compact=True, window=16, fault=None, compressed=False):
        self.bridge.bridge_init(window)
        port = Port(self.bridge, self.clock, fault)
        link = dlp.DisplayLink(port, timeout=.5, compact=compact, compressed=compressed)
        self.assertEqual(link.connect(), (64, 32))
        self.assertEqual(link.window_size, window)
        return link, port

    def check_image(self, image, count):
        self.assertEqual(self.bridge.bridge_presentations(), count)
        shown = ctypes.create_string_buffer(256)
        self.bridge.bridge_image(shown)
        self.assertEqual(shown.raw, image)

    def test_compression_faults_and_framing(self):
        for compact in (False, True):
            for window in (0, 1, 16):
                for fault in (None, "loss", "duplicate", "corrupt", "reorder", "ack", "completion") if window else (None,):
                    if window == 1 and fault == "reorder":
                        continue  # Only one packet can be in flight.
                    with self.subTest(compact=compact, window=window, fault=fault):
                        link, port = self.client(compact, window, fault, compressed=True)
                        self.assertTrue(link.capabilities & dlp.LZ4)
                        image = bytes(range(16)) * 16
                        link.send_frame(image)
                        self.check_image(image, 1)
                        self.assertIn(dlp.RECT_LZ4, [cell.opcode for cell in port.requests])
                        changed = bytes([image[0] ^ 1]) + image[1:]
                        link.send_frame(changed)
                        self.check_image(changed, 2)
                        link.connect()
                        link.send_frame(changed)
                        self.check_image(changed, 3)

    def test_faults_and_framing(self):
        for compact in (False, True):
            for fault in (None, "loss", "duplicate", "corrupt", "reorder", "ack", "completion"):
                with self.subTest(compact=compact, fault=fault):
                    link, port = self.client(compact, fault=fault)
                    image = bytes((k * 47 + k // 17) % 256 for k in range(256))
                    link.send_frame(image)
                    self.check_image(image, 1)
                    self.assertTrue(link.last_frame_stats.presented)
                    self.assertEqual(link.previous, image)
                    link.send_frame(image)
                    self.assertFalse(link.last_frame_stats.presented)
                    changed = bytes([image[0] ^ 1]) + image[1:]
                    link.send_frame(changed)
                    self.check_image(changed, 2)
                    link.connect()
                    link.send_frame(changed)
                    self.check_image(changed, 3)

    def test_single_packet_window_and_legacy_peer(self):
        for window in (0, 1):
            link, port = self.client(window=window)
            link.send_frame(bytes(256))
            self.check_image(bytes(256), 1)

    def test_lost_retained_content_starts_new_session(self):
        link, port = self.client()
        link.send_frame(bytes(256))
        old_session = link.session
        self.bridge.bridge_fault(1)
        image = b"\x01" + bytes(255)
        link.send_frame(image)
        self.assertNotEqual(link.session, old_session)
        self.check_image(image, 2)

    def test_failed_presentation_and_reset_invalidate_cache(self):
        for fault in (2, 3):
            link, port = self.client()
            link.send_frame(bytes(256))
            self.bridge.bridge_fault(fault)
            with self.assertRaises(RuntimeError):
                link.send_frame(b"\x01" + bytes(255))
            self.assertIsNone(link.previous)
            self.assertFalse(link.last_frame_stats.presented)
            self.assertTrue(link.window_failed)
            self.assertEqual(self.bridge.bridge_presentations(), 1)

    def test_small_frame_requires_one_completion_round_trip(self):
        link, port = self.client()
        start = len(port.requests)
        link.send_frame(bytes(256))
        controls = [c.opcode for c in port.requests[start:] if c.kind == 1]
        self.assertEqual(controls, [dlp.WINDOW_SYNC])
        self.check_image(bytes(256), 1)

    def test_malformed_completion_never_commits_host_cache(self):
        for offset, value in ((4, 0xffffffff), (28, 1)):
            link, port = self.client()
            command = link.command

            def bad_progress(opcode, payload=b"", attempts=3, **kwargs):
                answer = command(opcode, payload, attempts, **kwargs)
                if opcode == dlp.WINDOW_SYNC:
                    data = bytearray(answer.payload)
                    struct.pack_into("<I", data, offset, value)
                    answer = replace(answer, payload=bytes(data))
                return answer

            with patch.object(link, "command", bad_progress), self.assertRaises(ValueError):
                link.send_frame(bytes(256))
            self.assertIsNone(link.previous)
            self.assertFalse(link.last_frame_stats.presented)
            self.assertTrue(link.window_failed)


if __name__ == "__main__":
    unittest.main()
