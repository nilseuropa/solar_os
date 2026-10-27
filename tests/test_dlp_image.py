"""Check sender phase accounting and CLI reporting without serial hardware."""
from contextlib import redirect_stderr
import io
from pathlib import Path
import struct
import socket
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import dlp_image as dlp


class Clock:
    def __init__(self):
        self.now = 10.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class TimedPeer(dlp.DisplayLink):
    """A configured peer with deterministic upload and completion delays."""
    def __init__(self, port=None, compact=True, *, compressed=False):
        super().__init__(port, compact=compact, compressed=compressed)
        self.width, self.height = 16, 8
        self.max_rect_bytes = 16
        self.capabilities = dlp.PRESERVE_CONTENT | dlp.RLE | dlp.SYNC
        self.fail = False

    def connect(self):
        self.previous = None
        self.frame = 0
        return self.width, self.height

    def command(self, opcode, payload=b"", attempts=3):
        if self.fail:
            raise OSError("stream failed")
        # Each protocol transaction consumes host wall time. Completion waits
        # cost longer than uploads and must be attributed to present_wait.
        clock.sleep(.011 if opcode == dlp.GET_STATUS else .003 if opcode == dlp.PRESENT else .002)
        self.tx_bytes += 36 + len(payload)
        self.rx_bytes += 36
        if opcode == dlp.GET_STATUS:
            payload = bytes(8) + struct.pack("<I", self.frame) + bytes(12)
        elif opcode == dlp.GET_SYNC:
            payload = bytes(4)
        else:
            payload = b""
        return SimpleNamespace(payload=payload)


clock = None


class DlpImageTest(unittest.TestCase):
    def setUp(self):
        global clock
        clock = Clock()
        self.monotonic = patch.object(dlp.time, "monotonic", clock.monotonic)
        self.sleep = patch.object(dlp.time, "sleep", clock.sleep)
        self.monotonic.start()
        self.sleep.start()
        self.addCleanup(self.monotonic.stop)
        self.addCleanup(self.sleep.stop)

    def test_upload_and_completion_are_separate(self):
        peer = TimedPeer()
        self.assertEqual(peer.send_frame(bytes(16)), 1)
        stats = peer.last_frame_stats
        self.assertTrue(stats.presented)
        self.assertAlmostEqual(stats.upload_ms, 6)
        self.assertAlmostEqual(stats.present_wait_ms, 14)
        self.assertAlmostEqual(stats.total_ms, 20)
        self.assertEqual(peer.send_frame(bytes(16)), 1)
        self.assertFalse(peer.last_frame_stats.presented)
        self.assertAlmostEqual(peer.last_frame_stats.upload_ms, 2)
        self.assertEqual(peer.last_frame_stats.present_wait_ms, 0)
        self.assertAlmostEqual(peer.last_frame_stats.total_ms, 2)

    def test_failed_frame_does_not_reuse_previous_statistics(self):
        peer = TimedPeer()
        peer.send_frame(bytes(16))
        peer.fail = True
        with self.assertRaises(OSError):
            peer.send_frame(bytes([1]) + bytes(15))
        self.assertFalse(peer.last_frame_stats.presented)
        self.assertEqual(peer.last_frame_stats.present_wait_ms, 0)
        self.assertIsNone(peer.previous)

    def run_cli(self, interrupt=False, tcp=False):
        def frames(width, height):
            clock.sleep(.005)  # Preparation must count against the frame budget.
            yield bytes(16)
            clock.sleep(.005)
            yield bytes(16)
            if interrupt:
                raise KeyboardInterrupt

        port = object()
        # Context manager special methods are looked up on the type.
        class Port:
            def __enter__(self): return port
            def __exit__(self, *args): pass

        output = io.StringIO()
        args = ["dlp_image.py", "--demo", "--fps", "30", "--stats"]
        if tcp:
            args += ["--tcp", "127.0.0.1:7777"]
        with patch.object(sys, "argv", args), patch.object(dlp, "DisplayLink", TimedPeer), \
                patch.object(dlp, "TcpStream", lambda endpoint: Port()), \
                patch.object(dlp, "demo_frames", frames), \
                patch.dict(sys.modules, {"serial": None if tcp else SimpleNamespace(Serial=lambda *a, **k: Port()),
                                        "PIL": SimpleNamespace(Image=object())}), \
                redirect_stderr(output):
            result = dlp.main()
        return result, output.getvalue()

    def test_cli_counts_presented_and_unchanged_and_reports_phases(self):
        result, output = self.run_cli()
        self.assertEqual(result, 0)
        self.assertIn("prep=5.00ms upload=6.00ms present_wait=14.00ms total=25.00ms", output)
        self.assertIn("Summary: inputs=2 presented=1 unchanged=1", output)
        self.assertIn("(unchanged)", output)
        self.assertIn("elapsed=0.040s completed_fps=24.79", output)

    def test_ctrl_c_retains_summary(self):
        result, output = self.run_cli(interrupt=True)
        self.assertEqual(result, 130)
        self.assertIn("Summary: inputs=2 presented=1 unchanged=1", output)

    def test_tcp_cli_does_not_require_pyserial(self):
        result, output = self.run_cli(tcp=True)
        self.assertEqual(result, 0)
        self.assertIn("Opening Display Link on TCP 127.0.0.1:7777", output)
        self.assertIn("Summary: inputs=2 presented=1 unchanged=1", output)

    def test_tcp_endpoint_and_binary_adapter(self):
        for value in ("host", "host:0", ":7777", "host:65536", "host:no"):
            with self.assertRaises(dlp.argparse.ArgumentTypeError):
                dlp.tcp_endpoint(value)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            endpoint = dlp.tcp_endpoint("127.0.0.1:" + str(listener.getsockname()[1]))
            with dlp.TcpStream(endpoint) as stream, listener.accept()[0] as peer:
                peer.settimeout(1)
                payload = bytes(range(256))
                peer.sendall(payload[:7])
                self.assertEqual(stream.read(256), payload[:7])
                peer.sendall(payload[7:])
                self.assertEqual(stream.read(256), payload[7:])
                self.assertEqual(stream.write(payload), 256)
                self.assertEqual(peer.recv(256), payload)
                self.assertEqual(stream.read(256), b"")  # Bounded idle read.
                peer.shutdown(socket.SHUT_WR)
                with self.assertRaises(OSError):
                    stream.read(256)


if __name__ == "__main__":
    unittest.main()
