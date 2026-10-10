"""Run a model bundle on pictures, named tensor files, camera, or JPEG RTSP."""
import gc
import binascii
import hashlib
import json
import sys
import solaros
from model_bundle import ModelBundle, MANIFEST_MAX, require


def json_input_file(path):
    with open(path) as source:
        data = source.read(MANIFEST_MAX + 1)
    require(len(data) <= MANIFEST_MAX, "input map exceeds 64 KiB")
    mapping = json.loads(data)
    require(isinstance(mapping, dict), "expected input name to file map")
    folder = path.rsplit("/", 1)[0] if "/" in path else "."
    for name, value in mapping.items():
        require(isinstance(value, str) and value and "\x00" not in value, "expected input filename")
        mapping[name] = value if value.startswith("/") else folder + "/" + value
    return mapping


def raw_json(payload):
    if payload["kind"] != "raw":
        return payload
    tensors = {}
    for name, port in payload["tensors"].items():
        metadata = {key: port[key] for key in ("dtype", "shape", "exponents", "bytes")}
        data = port["data"]
        if len(data) <= 4096:
            metadata["encoding"], metadata["data"] = "hex", binascii.hexlify(data).decode()
        else:
            metadata["encoding"] = "sha256"
            metadata["data"] = binascii.hexlify(hashlib.sha256(data).digest()).decode()
        tensors[name] = metadata
    return {"kind": "raw", "tensors": tensors}


def record(bundle, source, sequence, result, frame, decode_ms, dropped=0, errors=0):
    timing = {key: result[key] for key in ("input_us", "inference_us", "output_us", "elapsed_us",
                                         "call_ms", "preprocess_us", "postprocess_ms")}
    timing["postprocess_us"] = result.get("postprocess_us", result["postprocess_ms"] * 1000)
    timing["decode_ms"] = decode_ms
    if frame:
        timing["source_latency_ms"] = max(0, solaros.time.uptime_ms() - frame["timestamp_us"] // 1000)
    return {"schema": 1, "event": "inference", "model_id": bundle.manifest["id"],
            "model_version": bundle.manifest["version"], "model_handle": bundle.handle, "source": source, "sequence": sequence,
            "frame": frame, "result": raw_json(result["result"]), "timings": timing,
            "transforms": result["transforms"], "stale_frames_dropped": dropped, "decode_errors": errors}


def files(bundle, source, count):
    ports = bundle.manifest["inputs"]
    if len(ports) == 1 and next(iter(ports.values()))["adapter"]["type"] == "image":
        paths = {next(iter(ports)): source}
    else:
        paths = json_input_file(source)
    require(set(paths) == set(ports), "all named input files required")
    handles, values = [], {}
    begin = solaros.time.uptime_ms()
    try:
        for name, path in paths.items():
            if ports[name]["adapter"]["type"] == "image":
                handle = solaros.image.open(path)
                handles.append(handle)
                values[name] = handle
            elif ports[name]["adapter"]["type"] == "tensor":
                expected = bundle.info["inputs"][name]["bytes"]
                with open(path, "rb") as file:
                    values[name] = file.read(expected)
                    require(len(values[name]) == expected and not file.read(1), "input size differs from model port")
            else:
                raise ValueError("file runner needs image/tensor adapter")
        decode_ms = solaros.time.uptime_ms() - begin
        for sequence in range(count or 1):
            gc.collect()
            result = bundle.run(values)
            print(json.dumps(record(bundle, source, sequence, result, None, decode_ms if sequence == 0 else 0)))
            del result
    finally:
        for handle in handles:
            solaros.image.close(handle)


def stream(bundle, source, count, max_age):
    ports = bundle.manifest["inputs"]
    require(len(ports) == 1 and next(iter(ports.values()))["adapter"]["type"] == "image",
            "stream runner needs one image input")
    name = next(iter(ports))
    network = source.startswith("rtsp://")
    if network:
        handle = solaros.rtsp.open(source, True, False)
    else:
        handle = solaros.streams.open(source[len("stream:"):])
    sequence, dropped, errors, consecutive_errors = 0, 0, 0, 0
    try:
        while not count or sequence < count:
            gc.collect()
            frame = solaros.rtsp.read_frame(handle, 1000) if network else solaros.streams.acquire_frame(handle)
            if frame is None:
                status = solaros.rtsp.status(handle)
                if status["ended"]:
                    raise OSError(status.get("error_detail", "RTSP session ended"))
                continue
            image = None
            begin = solaros.time.uptime_ms()
            try:
                metadata = solaros.streams.frame_info(frame)
                age = max(0, begin - metadata["timestamp_us"] // 1000)
                if max_age and age > max_age:
                    dropped += 1
                    continue
                try:
                    image = solaros.image.from_frame(frame)
                except (OSError, ValueError):
                    errors += 1
                    consecutive_errors += 1
                    if consecutive_errors >= 5:
                        raise
                    continue
            finally:
                try:
                    solaros.streams.release_frame(frame)
                except BaseException:
                    if image is not None:
                        solaros.image.close(image)
                    raise
            # The compressed frame lease is returned before preprocessing/inference.
            try:
                consecutive_errors = 0
                decode_ms = solaros.time.uptime_ms() - begin
                result = bundle.run({name: image})
                print(json.dumps(record(bundle, source, sequence, result, metadata, decode_ms, dropped, errors)))
                sequence += 1
                del result
            finally:
                solaros.image.close(image)
            solaros.time.sleep_ms(0)
    finally:
        if network:
            solaros.rtsp.close(handle)
        else:
            solaros.streams.close(handle)


def main(argv):
    if len(argv) < 3:
        print("Usage: python " + argv[0] + " BUNDLE_OR_HANDLE SOURCE [--count N] [--mode single|auto|dual] [--max-age MS]")
        return
    count, mode, max_age = None, "single", 250
    index = 3
    while index < len(argv):
        key = argv[index]
        require(index + 1 < len(argv), "missing option value")
        value = argv[index + 1]
        if key == "--count":
            count = int(value)
            require(0 <= count <= 10000, "count must be 0..10000")
        elif key == "--mode":
            mode = value
        elif key == "--max-age":
            max_age = int(value)
            require(0 <= max_age <= 60000, "max-age must be 0..60000 ms")
        else:
            raise ValueError("unknown option: " + key)
        index += 2
    live = argv[2].startswith("rtsp://") or argv[2].startswith("stream:")
    if count is None:
        count = 0 if live else 1
    with ModelBundle(argv[1], mode) as bundle:
        if live:
            stream(bundle, argv[2], count, max_age)
        else:
            files(bundle, argv[2], count)


if __name__ == "__main__":
    main(sys.argv)
