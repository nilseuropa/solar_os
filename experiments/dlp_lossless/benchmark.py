#!/usr/bin/env python3
"""Compare exact RGB565 DLP uploads on a USB-connected SolarOS receiver.

Uses a separate network console for setup/cleanup. No firmware or persistent
device settings are changed. Each measured update waits for PRESENT completion.
"""
import argparse
from collections import Counter
import ctypes
import json
from pathlib import Path
import random
import statistics
import struct
import sys
import time

import serial
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "linux-dlp-driver"))
sys.path.insert(0, str(ROOT / "solar_os/tests/hil"))
from linux_dlp_driver import protocol as dlp
from linux_dlp_driver.pixels import pack_image
from model_bundle_stream import Console


def tiles(before, after, width, height, size=16):
    """Exact changed tiles, merged horizontally within each tile row."""
    stride = width * 2
    result = []
    for y in range(0, height, size):
        h = min(size, height - y)
        start = None
        for x in range(0, width + size, size):
            w = min(size, width - x)
            changed = x < width and any(
                before[(y + k) * stride + x * 2:(y + k) * stride + (x + w) * 2]
                != after[(y + k) * stride + x * 2:(y + k) * stride + (x + w) * 2]
                for k in range(h))
            if changed and start is None:
                start = x
            if not changed and start is not None:
                result.append((start, y, min(x, width) - start, h))
                start = None
    return result


def scenes(width, height):
    background = Image.new("RGB", (width, height), "#eef2e8")
    draw = ImageDraw.Draw(background)
    draw.rectangle((0, 0, width - 1, 27), fill="#183e56")
    draw.text((8, 8), "DLP exact color experiment", fill="white")
    for y in range(42, height - 15, 18):
        draw.text((12, y), f"{y:03}  SolarOS display link / scrolling text", fill="#254228")
    sparse = []
    scrolling = []
    gradients = []
    noise = []
    rng = random.Random(91735)
    for step in range(4):
        image = background.copy()
        d = ImageDraw.Draw(image)
        color = ("#c02635", "#185fa7")[step % 2]
        for x in (8, width - 24):
            d.rectangle((x, 34, x + 12, 46), fill=color)
        sparse.append(image)
        image = Image.new("RGB", (width, height), "#eef2e8")
        d = ImageDraw.Draw(image)
        for row, y in enumerate(range(4, height, 16)):
            d.text((8, y), f"{row + step:03}  The quick brown fox / 0123456789", fill="#254228")
        scrolling.append(image)
        gradients.append(Image.frombytes("RGB", (width, height), bytes(
            c for y in range(height) for x in range(width)
            for c in ((x + step * 11) % 256, y % 256, (x + y + step * 7) % 256))))
        noise.append(Image.frombytes("RGB", (width, height), rng.randbytes(width * height * 3)))
    images = {
        "solid": [Image.new("RGB", (width, height), c) for c in ("red", "blue", "red", "blue")],
        "sparse-ui": sparse, "scroll": scrolling, "gradient": gradients, "noise": noise,
    }
    return {name: [pack_image(image, (width, height), pixel_format=dlp.RGB565)
                   for image in frames] for name, frames in images.items()}


class Lz4:
    def __init__(self):
        self.lib = ctypes.CDLL("liblz4.so.1")
        self.lib.LZ4_compress_default.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        self.lib.LZ4_decompress_safe.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]

    def encode(self, data):
        output = ctypes.create_string_buffer(len(data) + len(data) // 255 + 16)
        n = self.lib.LZ4_compress_default(data, output, len(data), len(output))
        if n <= 0:
            raise RuntimeError("LZ4 encoding failed")
        encoded = output.raw[:n]
        decoded = ctypes.create_string_buffer(len(data))
        assert self.lib.LZ4_decompress_safe(encoded, decoded, n, len(data)) == len(data)
        assert decoded.raw == data
        return encoded


def plan(before, image, width, height, mode, max_rect_bytes, lz4=None):
    full = before is None or mode.endswith("full")
    whole = [(0, 0, width, height)]
    rects = whole if full else (
        tiles(before, image, width, height) if "tiles" in mode else
        dlp.changed_rects(before, image, width, height, dlp.RGB565))
    def bands(rects):
        for x, y, w, h in rects:
            rows = max_rect_bytes // (w * 2)
            for offset in range(0, h, rows):
                yield x, y + offset, w, min(rows, h - offset)
    rects = list(bands(rects))
    cost = lambda rs: sum(1 + (w * h * 2 + 207) // 208 for x, y, w, h in rs)
    if not full and cost(rects) >= cost(list(bands(whole))):
        full, rects = True, list(bands(whole))
    commands = []
    decoded_bytes = 0
    decoded_regions = []
    for rect_id, (x, y, w, h) in enumerate(rects, 1):
        commands.append((dlp.BEGIN_RECT, rect_id, struct.pack("<HHHH", x, y, w, h)))
        data = b"".join(image[((y + k) * width + x) * 2:((y + k) * width + x + w) * 2]
                        for k in range(h))
        decoded_bytes += len(data)
        decoded_regions.append(data)
        offset = 0
        while offset < len(data):
            raw = min(208, len(data) - offset)
            used, op, payload = raw, dlp.RECT_DATA, data[offset:offset + raw]
            if not mode.startswith("raw"):
                encoded, consumed = dlp.rle_encode(data[offset:offset + 1024])
                if consumed > raw or (consumed == raw and len(encoded) + 2 < raw):
                    used, op, payload = consumed, dlp.RECT_RLE, struct.pack("<H", consumed) + encoded
            if mode.startswith("lz4"):
                block = data[offset:offset + 1024]
                encoded = lz4.encode(block)
                # A smaller independent block can fit one cell even when the
                # 1 KiB candidate cannot. Prefer expansion per cell for SPI.
                while len(encoded) > 206 and len(block) > raw:
                    block = block[:max(raw, len(block) // 2)]
                    encoded = lz4.encode(block)
                # Trial opcode is deliberately outside production support.
                if len(encoded) <= 206 and (len(block) > used or
                        (len(block) == used and len(encoded) + 2 < len(payload))):
                    used, op, payload = len(block), 0x16, struct.pack("<H", len(block)) + encoded
            commands.append((op, rect_id, struct.pack("<I", offset) + payload))
            offset += used
    return full, commands, len(rects), decoded_bytes, decoded_regions


def upload(link, prepared):
    full, commands = prepared[:2]
    link.frame += 1
    frame = link.frame
    link._upload_command(dlp.BEGIN_FRAME, struct.pack("<II", frame, int(full)))
    for op, rect_id, payload in commands:
        link._upload_command(op, struct.pack("<II", frame, rect_id) + payload)
    link._upload_command(dlp.PRESENT, struct.pack("<I", frame))
    if not link.window_size:
        raise RuntimeError("This experiment requires windowed PRESENT completion")


def verify(link, prepared):
    expected = 2166136261
    for data in prepared[4]:
        for byte in data:
            expected = ((expected ^ byte) * 16777619) & 0xffffffff
    version, limit, actual, size = struct.unpack("<HHII", link.command(8).payload)
    assert (version, limit, actual, size) == (1, 1024, expected, prepared[3]), (
        version, limit, actual, size, expected, prepared[3])
    return {"decoded_fnv1a": actual, "decoded_bytes_checked": size}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--display", default="display0")
    parser.add_argument("--stream", default="cdc0")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--modes", default="raw-full,rle-full,rle-bands,rle-tiles")
    parser.add_argument("--scenes", default="solid,sparse-ui,scroll,gradient,noise")
    parser.add_argument("--offline-only", action="store_true")
    parser.add_argument("--verify", action="store_true", help="Require the trial receiver's decoded-byte checksum")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    console = Console(args.host)
    log = []
    started_receiver = False
    def command(text):
        answer = console.command(text, 10)
        log.append(text + "\n" + answer)
        return answer
    try:
        identity = {name: command(name) for name in ("version", "board", "dlp status")}
        assert "DLP: stopped" in identity["dlp status"], "Receiver already owned"
        answer = command(f"dlp serve {args.display} stream={args.stream}")
        assert "DLP session" in answer, answer
        started_receiver = True
        with serial.Serial(args.port, 115200, timeout=.002, write_timeout=5, exclusive=True) as port:
            link = dlp.DisplayLink(port, timeout=5, pixel_format="rgb565")
            width, height = link.connect()
            assert link.window_size == 16
            print(f"RGB565 {width}x{height}, window={link.window_size}", flush=True)
            (args.output / "device.json").write_text(json.dumps({**identity, "width": width, "height": height,
                "format": "RGB565_BE", "window": link.window_size}, indent=2))
            corpus = scenes(width, height)
            lz4 = Lz4()
            offline = []
            records = []
            with (args.output / "frames.jsonl").open("w") as results:
                for name in args.scenes.split(","):
                    frames = corpus[name]
                    for mode in args.modes.split(","):
                        if args.offline_only:
                            break
                        link.connect()
                        if mode.startswith("lz4"):
                            answer = link.command(8).payload
                            assert answer[:4] == struct.pack("<HH", 1, 1024), answer
                        upload(link, plan(None, frames[0], width, height, "rle-full", link.max_rect_bytes))
                        for index, image in enumerate(frames[1:], 1):
                            at = time.perf_counter()
                            prepared = plan(frames[index - 1], image, width, height, mode, link.max_rect_bytes, lz4)
                            encode_ms = (time.perf_counter() - at) * 1000
                            tx, rx = link.tx_bytes, link.rx_bytes
                            at = time.perf_counter()
                            upload(link, prepared)
                            transfer_ms = (time.perf_counter() - at) * 1000
                            record = {"scene": name, "mode": mode, "index": index,
                                "encode_ms": encode_ms, "transfer_present_ms": transfer_ms,
                                "total_ms": encode_ms + transfer_ms, "tx_bytes": link.tx_bytes - tx,
                                "rx_bytes": link.rx_bytes - rx, "rectangles": prepared[2],
                                "updated_bytes": prepared[3], "opcodes": dict(Counter(hex(c[0]) for c in prepared[1]))}
                            if args.verify:
                                record.update(verify(link, prepared))
                            records.append(record)
                            results.write(json.dumps(record) + "\n"); results.flush()
                            print(json.dumps(record), flush=True)
                    for mode in ("rle-full", "lz4-full", "rle-bands", "rle-tiles", "lz4-tiles"):
                        at = time.perf_counter()
                        p = plan(frames[0], frames[1], width, height, mode, link.max_rect_bytes, lz4)
                        # Exact request-cell size, excluding window checkpoints and BEGIN/PRESENT.
                        wire = sum(36 + 8 + len(c[2]) for c in p[1])
                        offline.append({"scene": name, "mode": mode, "planned_bytes": wire,
                            "commands": len(p[1]), "rectangles": p[2], "updated_bytes": p[3],
                            "encode_verify_ms": (time.perf_counter() - at) * 1000})
            (args.output / "offline.json").write_text(json.dumps(offline, indent=2))
            summary = []
            for name in args.scenes.split(","):
                for mode in args.modes.split(","):
                    rows = [r for r in records if r["scene"] == name and r["mode"] == mode]
                    if rows:
                        summary.append({"scene": name, "mode": mode, "samples": len(rows),
                            **{key: statistics.median(r[key] for r in rows) for key in
                               ("total_ms", "transfer_present_ms", "encode_ms", "tx_bytes", "rectangles")}})
            (args.output / "summary.json").write_text(json.dumps(summary, indent=2))
        log.append(command("dlp status"))
    finally:
        if started_receiver:
            command(f"dlp stop {args.display}")
            command("dlp status")
            command("session list")
        console.socket.close()
        (args.output / "console.log").write_text("\n".join(log))


if __name__ == "__main__":
    main()
