#!/usr/bin/env python3
"""Benchmark a direct USB Pico RAM endpoint using the same exact RGB565 corpus."""
import argparse
from collections import Counter
import json
from pathlib import Path
import statistics
import struct
import time

import serial
from benchmark import Lz4, dlp, plan, scenes, upload


def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--modes", default="raw-full,rle-full,rle-bands,rle-tiles,lz4-full,lz4-bands,lz4-tiles")
    parser.add_argument("--scenes", default="solid,sparse-ui,scroll,gradient,noise")
    parser.add_argument("--fixed", action="store_true")
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    records = []
    with serial.Serial(args.port, 115200, timeout=.002, write_timeout=5, exclusive=True) as port:
        link = dlp.DisplayLink(port, timeout=5, pixel_format="rgb565", compact=not args.fixed)
        width, height = link.connect()
        assert (width, height, link.window_size) == (320, 240, 16)
        descriptor = link.command(dlp.GET_INFO).payload
        model = descriptor[60:92].split(b"\0")[0].decode()
        assert model == "Pico RAM display", model
        metadata = {"width": width, "height": height, "model": model, "format": "RGB565_BE",
            "framing": "fixed" if args.fixed else "compact", "window": link.window_size,
            "backend": "RAM", "clock_limit_advertised": struct.unpack_from("<I", descriptor, 20)[0]}
        if args.verify:
            query = link.command(8).payload
            assert struct.unpack_from("<HH", query) == (1, 1024)
            metadata["cpu_hz"] = struct.unpack_from("<I", query, 24)[0]
        (args.output / "device.json").write_text(json.dumps(metadata, indent=2) + "\n")
        print(json.dumps(metadata), flush=True)
        corpus = scenes(width, height)
        lz4 = Lz4()
        with (args.output / "frames.jsonl").open("w") as output:
            for name in args.scenes.split(","):
                frames = corpus[name]
                for mode in args.modes.split(","):
                    link.connect()
                    if mode.startswith("lz4"):
                        assert struct.unpack_from("<HH", link.command(8).payload) == (1, 1024)
                    upload(link, plan(None, frames[0], width, height, "rle-full", link.max_rect_bytes))
                    for index, image in enumerate(frames[1:], 1):
                        started = time.perf_counter()
                        prepared = plan(frames[index - 1], image, width, height, mode, link.max_rect_bytes, lz4)
                        encode_ms = (time.perf_counter() - started) * 1000
                        tx, rx = link.tx_bytes, link.rx_bytes
                        started = time.perf_counter()
                        upload(link, prepared)
                        transfer_ms = (time.perf_counter() - started) * 1000
                        row = {"scene": name, "mode": mode, "index": index,
                            "encode_ms": encode_ms, "transfer_present_ms": transfer_ms,
                            "total_ms": encode_ms + transfer_ms, "tx_bytes": link.tx_bytes - tx,
                            "rx_bytes": link.rx_bytes - rx, "rectangles": prepared[2],
                            "updated_bytes": prepared[3], "opcodes": dict(Counter(hex(c[0]) for c in prepared[1]))}
                        if args.verify:
                            version, limit, actual, size, decode_us, write_us, hash_us, cpu_hz = struct.unpack(
                                "<HHIIIIII", link.command(8).payload)
                            assert (version, limit, actual, size) == (1, 1024,
                                fnv(b"".join(prepared[4])), prepared[3])
                            actual_image = struct.unpack("<I", link.command(9).payload)[0]
                            assert actual_image == fnv(image), (actual_image, fnv(image))
                            row.update(decoded_fnv1a=actual, decoded_bytes_checked=size,
                                framebuffer_fnv1a=actual_image, decode_us=decode_us,
                                ram_write_us=write_us, verification_hash_us=hash_us, cpu_hz=cpu_hz)
                        records.append(row)
                        output.write(json.dumps(row) + "\n"); output.flush()
                        print(json.dumps(row), flush=True)
    summary = []
    for name in args.scenes.split(","):
        for mode in args.modes.split(","):
            samples = [r for r in records if r["scene"] == name and r["mode"] == mode]
            keys = ["transfer_present_ms", "encode_ms", "total_ms", "tx_bytes", "rectangles"]
            if args.verify:
                keys += ["decode_us", "ram_write_us", "verification_hash_us"]
            summary.append({"scene": name, "mode": mode, "samples": len(samples),
                            **{key: statistics.median(r[key] for r in samples) for key in keys}})
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
