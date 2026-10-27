#!/usr/bin/env python3
"""Test an installed DLP receiver using a separate network console and serial stream.

Requires pyserial and Pillow. Leaves the receiver stopped and restores the
previous display session. Protocol completion is not a physical screen capture.
"""
import argparse
from pathlib import Path
import re
import secrets
import struct
import subprocess
import sys
import time

import serial
from model_bundle_stream import Console

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from dlp_image import DisplayLink, demo_frames


def send_rgb565(link, width, height):
    """Exercise the advertised color path independently of the MONO1 sender."""
    descriptor = link.command(1).payload
    if not struct.unpack_from("<I", descriptor, 8)[0] & 16:
        return False
    link.session = secrets.randbelow(0xffffffff) + 1
    link.command(2, struct.pack("<HH", 4, 0))
    link.command(0x10, struct.pack("<II", 1, 1))
    link.command(0x11, struct.pack("<IIHHHH", 1, 1, 0, 0, width, height))
    colors = (0xffff, 0xf800, 0x07e0, 0x001f, 0xffe0, 0x07ff, 0xf81f, 0)
    row = b"".join(struct.pack(">H", colors[min(7, x * 8 // width)]) for x in range(width))
    image = row * height
    for offset in range(0, len(image), 208):
        link.command(0x12, struct.pack("<III", 1, 1, offset) + image[offset:offset + 208])
    link.command(0x13, struct.pack("<I", 1))
    deadline = time.monotonic() + link.present_timeout
    while time.monotonic() < deadline:
        status = link.command(3).payload
        assert not struct.unpack_from("<H", status, 2)[0], status
        if struct.unpack_from("<I", status, 8)[0] == 1:
            return True
        time.sleep(.01)
    raise TimeoutError("RGB565 presentation did not complete")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--stream", default="cdc0")
    parser.add_argument("--display", default="display0")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--no-window", action="store_true")
    parser.add_argument("--benchmark", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    console = Console(args.host)
    logs = []
    running = False

    def command(text):
        output = console.command(text, 10)
        logs.append(text + "\n" + output)
        print(text + "\n" + output, flush=True)
        return output

    try:
        command("version")
        command("board")
        command("session list")
        command("mem policy")
        assert "DLP: stopped" in command("dlp status"), "A receiver is already running"
        assert "required" in command(f"dlp serve {args.display} stream=battery")
        assert "DLP: stopped" in command("dlp status")
        for compact in (True, False):
            output = command(f"dlp serve {args.display} stream={args.stream}")
            assert re.search(r"DLP session \d+:", output), output
            running = True
            # The separate console must remain usable and report stream ownership.
            assert "app-dlp" in command("port list")
            duplicate = command(f"dlp serve {args.display} stream={args.stream}")
            assert "owns" in duplicate, duplicate
            with serial.Serial(args.port, args.baud, timeout=.05, write_timeout=2) as port:
                link = DisplayLink(port, compact=compact)
                if args.no_window:
                    link.requested_window = 0
                width, height = link.connect()
                assert link.window_size == (0 if args.no_window else 16)
                frames = demo_frames(width, height)
                image = next(frames)
                started = time.monotonic()
                link.send_frame(image)
                sent = link.tx_bytes
                # An unchanged image is confirmed without another PRESENT.
                assert link.send_frame(image) == 1
                assert link.tx_bytes - sent <= 256
                link.send_frame(next(frames))
                link.connect()  # New session must repaint even for the same image.
                assert link.send_frame(image) == 1
                logs.append(f"{'compact' if compact else 'fixed'} {width}x{height}: "
                    f"3 presentations in {time.monotonic() - started:.3f}s; "
                    f"TX {link.tx_bytes}, RX {link.rx_bytes}\n")
            # Closing serial alone leaves an endpoint listening for another sender.
            status = command("dlp status")
            assert "frames=3" in status, status
            assert "Presentation timing: n=3" in status, status
            assert "  render:" in status and "  display:" in status and "  total:" in status, status
            assert "completion rate:" in status, status
            # Pause behind another display session, then restore the receiver.
            output = command(f"session create clock {args.display}")
            match = re.search(r"session (\d+) created", output)
            assert match, output
            try:
                assert "suspended" in command("dlp status")
            finally:
                command("session close " + match[1])
            resumed = command("dlp status")
            assert "serving" in resumed and "Presentation timing: n=3" in resumed, resumed
            with serial.Serial(args.port, args.baud, timeout=.05, write_timeout=2) as port:
                # Continue the same protocol session with a delta after redraw.
                link.port = port
                link.send_frame(next(frames))
                color = compact and send_rgb565(link, width, height)
                if color:
                    link.connect()
                    link.send_frame(image)  # RGB565 -> MONO1 must repaint fully.
            count = 6 if color else 4
            assert f"frames={count}" in command("dlp status")
            command(f"dlp stop {args.display}")
            running = False
            stopped = command("dlp status")
            assert "DLP: stopped" in stopped and f"Presentation timing: n={count}" in stopped, stopped
            ports = command("port list")
            assert "app-dlp" not in ports, ports
        if args.benchmark:
            assert "DLP session" in command(f"dlp serve {args.display} stream={args.stream}")
            running = True
            console.socket.close()
            try:
                result = subprocess.run([sys.executable, "scripts/dlp_image.py", "--port", args.port,
                    "--baud", str(args.baud), "--demo", "--fps", "30", "--frames", "100", "--stats"] +
                    (["--no-window"] if args.no_window else []),
                    capture_output=True, text=True, timeout=120)
            finally:
                console = Console(args.host)
            (args.output / "sender.log").write_text(result.stdout + result.stderr)
            assert result.returncode == 0, result.stderr
            assert "presented=100 unchanged=0" in result.stderr, result.stderr
            print(result.stderr.splitlines()[-1], flush=True)
            assert "Presentation timing: n=100" in command("dlp status")
            command(f"dlp stop {args.display}")
            running = False
        command("session list")
        command("mem policy")
        command("log show")
        print("DLP serial receiver lifecycle: PASS", flush=True)
    finally:
        if running:
            command(f"dlp stop {args.display}")
        (args.output / "console.log").write_text("\n".join(logs))
        console.socket.close()


if __name__ == "__main__":
    main()
