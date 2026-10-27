#!/usr/bin/env python3
"""Test DLP over a raw Wi-Fi TCP stream, with Telnet kept for control."""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

from model_bundle_stream import Console
from dlp_receiver import send_rgb565

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from dlp_image import DisplayLink, TcpStream, demo_frames


class DropUpload:
    """Discard one complete PUSH; the real receiver must request the gap."""
    def __init__(self, stream):
        self.stream, self.buffer, self.dropped = stream, bytearray(), False

    def read(self, size):
        return self.stream.read(size)

    def write(self, data):
        self.buffer.extend(data)
        while len(self.buffer) >= 32:
            size = (36 + struct.unpack_from("<H", self.buffer, 18)[0]
                    if self.buffer[:4] == b"DLP1" else 256)
            if len(self.buffer) < size:
                break
            packet = bytes(self.buffer[:size])
            del self.buffer[:size]
            if packet[6] == 3 and struct.unpack_from("<I", packet, 12)[0] == 3 and not self.dropped:
                self.dropped = True
                continue
            at, deadline = 0, time.monotonic() + 2
            while at < len(packet):
                at += self.stream.write(packet[at:])
                if time.monotonic() >= deadline:
                    raise TimeoutError("Fault-injection stream write stalled")
        return len(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=7777)
    parser.add_argument("--stream", default="dlptcp")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--no-window", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    deadline = time.monotonic() + 30
    while True:
        try:
            console = Console(args.host)
            break
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.5)
    logs = []
    created = running = False

    def command(text):
        output = console.command(text, 10)
        logs.append(text + "\n" + output)
        print(text + "\n" + output, flush=True)
        return output

    def wait_status(expected):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            status = command("dlp status")
            if f"DLP: {expected}" in status:
                return status
            time.sleep(.05)
        raise AssertionError(f"DLP did not become {expected}")

    def assert_display_shell():
        sessions = command("session list")
        assert re.search(r"shell\s+active\s+display0", sessions), sessions

    try:
        assert "DLP: stopped" in command("dlp status"), "receiver already running"
        assert "listening on port" in command(f"stream tcp listen {args.stream} {args.port}")
        created = True
        assert "Sharing: exclusive" in command(f"stream status {args.stream}")
        for compact in (True, False):
            assert "DLP session" in command(f"dlp serve display0 stream={args.stream}")
            running = True
            assert "DLP: waiting" in command("dlp status")
            assert "app-dlp" in command("port list")
            assert "removed" not in command(f"stream tcp remove {args.stream}")
            with TcpStream((args.host, args.port)) as stream:
                fault_stream = DropUpload(stream) if compact and not args.no_window else None
                link = DisplayLink(fault_stream or stream, compact=compact)
                if args.no_window:
                    link.requested_window = 0
                width, height = link.connect()
                assert link.window_size == (0 if args.no_window else 16)
                frames = demo_frames(width, height)
                first = next(frames)
                link.send_frame(first)
                if fault_stream:
                    assert fault_stream.dropped, "PUSH loss was not injected"
                sent = link.tx_bytes
                link.send_frame(first)
                assert not link.last_frame_stats.presented and link.tx_bytes - sent <= 256
                link.send_frame(next(frames))
                link.connect()
                link.send_frame(first)
                assert "frames=3" in command("dlp status")
                output = command("session create clock display0")
                match = re.search(r"session (\d+) created", output)
                assert match, output
                try:
                    assert "suspended" in command("dlp status")
                finally:
                    command("session close " + match[1])
                assert "Presentation timing: n=3" in command("dlp status")
                link.send_frame(next(frames))
                color = compact and send_rgb565(link, width, height)
                if color:
                    link.connect()
                    link.send_frame(first)
                count = 6 if color else 4
                assert f"frames={count}" in command("dlp status")
                old_boot = link.boot
            status = wait_status("waiting")
            assert f"Presentation timing: n={count}" in status
            assert "app-dlp" in command("port list")
            # Reconnect without starting another app. A new peer gets a fresh
            # boot/session, and an interrupted upload must not be presented.
            with TcpStream((args.host, args.port)) as stream:
                link = DisplayLink(stream, compact=compact)
                if args.no_window:
                    link.requested_window = 0
                width, height = link.connect()
                assert link.boot != old_boot
                link.send_frame(first)
                count += 1
                if compact:
                    command("session focus display0")
                    # Leave a persistent Alt switcher over DLP before exit.
                    # Virtual taps preserve modifiers until the next event.
                    command("input emit ALT+TAB")
                    assert "suspended" in command("dlp status")
                    command("input emit ALT+TAB")
                    assert "serving" in command("dlp status")
                    command("input emit CTRL+ALT+APP_EXIT")
                    wait_status("stopped")  # normal exit with a live provider
                else:
                    link.command(0x10, struct.pack("<II", 2, 1))
                    link.command(0x11, struct.pack("<IIHHHH", 2, 1, 0, 0, width, height))
                    link.command(0x12, struct.pack("<III", 2, 1, 0) + b"\xff")
            if not compact:
                status = wait_status("waiting")
                assert f"frames={count}" in status
                command("session focus display0")
                command("input emit ESC")
                wait_status("stopped")  # exit while awaiting another provider
            running = False
            assert_display_shell()
            assert "app-dlp" not in command("port list")
        assert "DLP session" in command(f"dlp serve display0 stream={args.stream}")
        running = True
        command("wifi status")
        # Measure a data-only connection; Telnet must not supply the Wi-Fi
        # latency lease on behalf of the TCP stream during this benchmark.
        console.socket.close()
        try:
            result = subprocess.run([sys.executable, "scripts/dlp_image.py", "--tcp",
                f"{args.host}:{args.port}", "--demo", "--fps", "30", "--frames", "100", "--stats"] +
                (["--no-window"] if args.no_window else []),
                capture_output=True, text=True, timeout=120)
        finally:
            console = Console(args.host)
        (args.output / "sender.log").write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stderr
        assert "presented=100 unchanged=0" in result.stderr, result.stderr
        print(result.stderr.splitlines()[-1], flush=True)
        assert "Presentation timing: n=100" in wait_status("waiting")
        command("dlp stop display0")
        wait_status("stopped")
        running = False
        assert_display_shell()
        command("session list")
        command("log show")
        print("DLP Wi-Fi TCP: compact/fixed, delta, resume, RGB, reconnect, interrupted upload, focused exit, remote stop and 100 frames PASS", flush=True)
    finally:
        try:
            if running:
                command("dlp stop display0")
            if created:
                command(f"stream tcp remove {args.stream}")
        finally:
            (args.output / "console.log").write_text("\n".join(logs))
            console.socket.close()


if __name__ == "__main__":
    main()
