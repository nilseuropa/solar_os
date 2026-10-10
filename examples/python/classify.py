"""SolarOS ImageNet MobileNetV2 classifier; see classify.README.md."""
import gc
import math
import struct
import sys
import solaros

MEAN = (123.675, 116.28, 103.53)
STD = (58.395, 57.12, 57.375)
WIDTH = HEIGHT = 224


def read_exact(source, length):
    data = source.read(length)
    if len(data) != length:
        raise ValueError("truncated image")
    return data


def check_size(width, height):
    if width <= 0 or height <= 0 or width * height > 2 * 1024 * 1024:
        raise ValueError("picture exceeds 2 million pixels or has invalid dimensions")


def read_portable_picture(path):
    """BMP/PPM fallback for firmware without image.to_rgb; bounded row reads."""
    with open(path, "rb") as source:
        magic = read_exact(source, 2)
        if magic == b"BM":
            header = magic + read_exact(source, 52)
            offset = struct.unpack("<I", header[10:14])[0]
            dib = struct.unpack("<I", header[14:18])[0]
            width, height = struct.unpack("<ii", header[18:26])
            planes, bits = struct.unpack("<HH", header[26:30])
            compression = struct.unpack("<I", header[30:34])[0]
            if dib < 40 or offset < 14 + dib or planes != 1 or bits != 24 or compression:
                raise ValueError("use uncompressed 24-bit BMP")
            bottom_up = height > 0
            height = abs(height)
            stride = (width * 3 + 3) & ~3
            bgr = True
        elif magic == b"P6":
            def token():
                value = b""
                while True:
                    ch = read_exact(source, 1)
                    if ch == b"#" and not value:
                        source.readline()
                    elif ch in b" \t\r\n":
                        if value:
                            if ch == b"\r":
                                following = source.read(1)
                                if following != b"\n":
                                    source.seek(-len(following), 1)
                            return value
                    else:
                        value += ch
                        if len(value) > 16:
                            raise ValueError("invalid PPM header")
            width, height, maximum = int(token()), int(token()), int(token())
            if maximum != 255:
                raise ValueError("use 8-bit RGB P6 PPM")
            offset = source.tell()
            stride = width * 3
            bottom_up = bgr = False
        else:
            raise ValueError("this firmware needs image.to_rgb for JPEG/PNG/GIF/WebP; "
                             "use a 24-bit BMP or P6 PPM picture")
        check_size(width, height)
        result = bytearray(WIDTH * HEIGHT * 3)
        xs = [x * width // WIDTH * 3 for x in range(WIDTH)]
        previous = -1
        for y in range(HEIGHT):
            sy = y * height // HEIGHT
            if bottom_up:
                sy = height - sy - 1
            if sy != previous:
                source.seek(offset + sy * stride)
                row = read_exact(source, width * 3)
                previous = sy
            base = y * WIDTH * 3
            for x in range(WIDTH):
                src, dst = xs[x], base + x * 3
                result[dst] = row[src + (2 if bgr else 0)]
                result[dst + 1] = row[src + 1]
                result[dst + 2] = row[src + (0 if bgr else 2)]
            solaros.time.sleep_ms(0)
        return result, (width, height)


def picture_rgb(path):
    suffix = path.lower()
    if suffix.endswith(".bmp") or suffix.endswith(".ppm"):
        return read_portable_picture(path)
    if hasattr(solaros, "image") and hasattr(solaros.image, "to_rgb"):
        handle = solaros.image.open(path)
        try:
            size = solaros.image.size(handle)
            return solaros.image.to_rgb(handle, WIDTH, HEIGHT), size
        finally:
            solaros.image.close(handle)
    return read_portable_picture(path)


def quantization_tables(exponent):
    tables = []
    scale = 2.0 ** exponent
    for mean, std in zip(MEAN, STD):
        table = bytearray(256)
        for value in range(256):
            quantized = int(math.floor((value - mean) / std / scale + 0.5))
            table[value] = max(-128, min(127, quantized)) & 255
        tables.append(table)
    return tables


def quantize_in_place(data, tables):
    red, green, blue = tables
    for offset in range(0, len(data), 3):
        data[offset] = red[data[offset]]
        data[offset + 1] = green[data[offset + 1]]
        data[offset + 2] = blue[data[offset + 2]]
        if offset % (WIDTH * 3 * 8) == 0:
            solaros.time.sleep_ms(0)


def top_classes(data, exponent, labels, count):
    if len(data) != len(labels):
        raise ValueError("output size does not match labels")
    # Signed quantized logits, stable softmax over all classes, then top-k.
    values = [value if value < 128 else value - 256 for value in data]
    maximum = max(values)
    scale = 2.0 ** exponent
    weights = [math.exp((value - maximum) * scale) for value in values]
    total = sum(weights)
    indices = sorted(range(len(values)), key=lambda i: values[i], reverse=True)[:count]
    return [{"id": i, "label": labels[i], "score": weights[i] / total} for i in indices]


def timing_summary(values):
    values = sorted(values)
    n = len(values)
    median = (values[(n - 1) // 2] + values[n // 2]) / 2
    return {"min": values[0], "median": median,
            "mean": sum(values) / n, "max": values[-1]}


def classify(model, info, labels, tables, path, repeat, warmup, top, timeout):
    clock = solaros.time.uptime_ms
    gc.collect()
    heap_before = gc.mem_free()
    started = clock()
    input_name = next(iter(info["inputs"]))
    native_preprocess_ms = None
    if (hasattr(solaros.inference, "prepare_image") and hasattr(solaros, "image") and
            not path.lower().endswith(".bmp") and not path.lower().endswith(".ppm")):
        image = solaros.image.open(path)
        try:
            size = solaros.image.size(image)
            after_decode = clock()
            prepared = solaros.inference.prepare_image(model, input_name, image,
                {"layout": "NHWC", "color": "RGB", "resize": "stretch", "mean": MEAN, "std": STD})
            data = prepared["data"]
            native_preprocess_ms = prepared["preprocess_us"] / 1000
            after_quantize = clock()
        finally:
            solaros.image.close(image)
    else:
        data, size = picture_rgb(path)
        after_decode = clock()
        quantize_in_place(data, tables)
        after_quantize = clock()
    output_name = next(iter(info["outputs"]))
    output_exponent = info["outputs"][output_name]["exponents"][0]
    durations = {"call_ms": [], "native_ms": [], "input_copy_ms": [],
                 "output_copy_ms": [], "native_elapsed_ms": []}
    result = None
    after_first_call = after_quantize
    for index in range(warmup + repeat):
        gc.collect()
        begin = clock()
        result = solaros.inference.run(model, {input_name: data}, timeout)
        elapsed = clock() - begin
        if index == 0:
            after_first_call = clock()
        if index >= warmup:
            durations["call_ms"].append(elapsed)
            for key, field in (("native_ms", "inference_us"), ("input_copy_ms", "input_us"),
                               ("output_copy_ms", "output_us"), ("native_elapsed_ms", "elapsed_us")):
                durations[key].append(result[field] / 1000)
    post_begin = clock()
    classes = top_classes(result["outputs"][output_name]["data"],
                          output_exponent, labels, top)
    finished = clock()
    report = {"image": path, "source_size": list(size), "top": classes,
              "decode_resize_ms": after_decode - started,
              "normalize_quantize_ms": after_quantize - after_decode,
              "first_picture_call_ms": after_first_call - started,
              "postprocess_ms": finished - post_begin,
              "total_batch_ms": finished - started,
              "repeat": repeat, "warmup": warmup,
              "timings": {key: timing_summary(v) for key, v in durations.items()},
              "python_heap_free_before": heap_before}
    report["preprocess_backend"] = "native" if native_preprocess_ms is not None else "python"
    report["native_preprocess_ms"] = native_preprocess_ms
    del data, result
    gc.collect()
    report["python_heap_free_after"] = gc.mem_free()
    return report


def main(argv):
    folder = argv[0].rsplit("/", 1)[0] if "/" in argv[0] else "."
    options = {"--model": folder + "/imagenet_cls_mobilenetv2_s8_v1.espdl",
               "--labels": folder + "/labels.txt", "--repeat": 1,
               "--warmup": 0, "--top": 5, "--timeout": 10000, "--mode": "single"}
    images, as_json, index = [], False, 1
    while index < len(argv):
        arg = argv[index]
        if arg == "--json":
            as_json = True
        elif arg in options:
            index += 1
            if index == len(argv):
                raise ValueError("missing value for " + arg)
            options[arg] = argv[index] if arg in ("--model", "--labels", "--mode") else int(argv[index])
        elif arg.startswith("--"):
            raise ValueError("unknown option " + arg)
        else:
            images.append(arg)
        index += 1
    if not images:
        print("Usage: python " + argv[0] + " PICTURE [PICTURE ...] [--repeat N] "
              "[--warmup N] [--top N] [--mode single|auto|dual] [--json]")
        return
    if not 1 <= options["--repeat"] <= 100 or not 0 <= options["--warmup"] <= 10:
        raise ValueError("repeat must be 1..100 and warmup 0..10")
    if not 1 <= options["--top"] <= 1000 or not 1 <= options["--timeout"] <= 60000:
        raise ValueError("top must be 1..1000 and timeout 1..60000 ms")
    if options["--mode"] not in ("single", "auto", "dual"):
        raise ValueError("mode must be single, auto or dual")
    with open(options["--labels"]) as source:
        labels = [line.strip() for line in source if line.strip()]
    if len(labels) != 1000:
        raise ValueError("expected the 1000 ordered ImageNet labels")
    gc.collect()
    begin = solaros.time.uptime_ms()
    model = solaros.inference.load(options["--model"], options["--timeout"])
    load_ms = solaros.time.uptime_ms() - begin
    try:
        if hasattr(solaros.inference, "set_mode"):
            solaros.inference.set_mode(model, options["--mode"])
        elif options["--mode"] != "single":
            raise ValueError("this firmware does not support execution mode selection")
        info = solaros.inference.info(model)
        if len(info["inputs"]) != 1 or len(info["outputs"]) != 1:
            raise ValueError("expected one image input and one classifier output")
        input_tensor = next(iter(info["inputs"].values()))
        output_tensor = next(iter(info["outputs"].values()))
        if (input_tensor["dtype"] != "int8" or input_tensor["shape"] != [1, HEIGHT, WIDTH, 3]
                or output_tensor["dtype"] != "int8" or output_tensor["shape"] != [1, 1000]
                or len(input_tensor["exponents"]) != 1 or len(output_tensor["exponents"]) != 1):
            raise ValueError("model does not match the MobileNetV2 ImageNet contract")
        tables = quantization_tables(input_tensor["exponents"][0])
        if not as_json:
            print("MobileNetV2 ImageNet: 1000 classes; model stays resident across pictures")
            print("Model load: %d ms; file %d bytes; resident heap delta SRAM %d / PSRAM %d bytes" %
                  (load_ms, info["model_bytes"], info["internal_bytes"], info["external_bytes"]))
        for path in images:
            report = classify(model, info, labels, tables, path, options["--repeat"],
                              options["--warmup"], options["--top"], options["--timeout"])
            report["model_load_ms"] = load_ms
            report["mode"] = options["--mode"]
            report["model_bytes"] = info["model_bytes"]
            report["resident_internal_bytes"] = info["internal_bytes"]
            report["resident_external_bytes"] = info["external_bytes"]
            if as_json:
                import json
                print(json.dumps(report))
            else:
                print("\nPicture: %s (%dx%d)" % (path, report["source_size"][0], report["source_size"][1]))
                for rank, item in enumerate(report["top"]):
                    print("%d. %6.2f%%  %s [class %d]" %
                          (rank + 1, item["score"] * 100, item["label"], item["id"]))
                print("Image read/decode%s: %d ms; tensor preparation: %d ms; softmax/top-k: %d ms" %
                      ("" if report["preprocess_backend"] == "native" else "/resize",
                       report["decode_resize_ms"], report["normalize_quantize_ms"], report["postprocess_ms"]))
                print("First picture call (excluding model load and softmax): %d ms; batch: %d ms" %
                      (report["first_picture_call_ms"], report["total_batch_ms"]))
                for key in ("call_ms", "native_ms", "input_copy_ms", "output_copy_ms", "native_elapsed_ms"):
                    values = report["timings"][key]
                    print("%s: min %.3f / median %.3f / mean %.3f / max %.3f" %
                          (key, values["min"], values["median"], values["mean"], values["max"]))
                print("Runs: %d measured + %d warmup; Python heap free: %d -> %d bytes" %
                      (report["repeat"], report["warmup"], report["python_heap_free_before"],
                       report["python_heap_free_after"]))
    finally:
        solaros.inference.close(model)


if __name__ == "__main__":
    main(sys.argv)
