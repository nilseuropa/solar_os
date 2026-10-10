"""Resident SolarOS model bundles. Native inference remains usable independently."""
import binascii
import gc
import hashlib
import json
import math
try:
    import solaros
except ImportError:
    solaros = None  # Allows the host bundle builder to use the same validator.

MANIFEST_MAX = 65536
TENSOR_MAX = 4 * 1024 * 1024
DTYPE_BYTES = {"int8": 1, "uint8": 1, "int16": 2, "uint16": 2,
               "int32": 4, "uint32": 4, "int64": 8, "uint64": 8,
               "float32": 4, "float64": 8, "float16": 2, "bool": 1}
INPUT_ADAPTERS = {}
RESULT_ADAPTERS = {}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def keys(value, allowed, required=()):
    require(isinstance(value, dict), "expected object")
    require(all(key in allowed for key in value), "unknown bundle field")
    require(all(key in value for key in required), "missing bundle field")


def integer(value, low, high):
    return type(value) is int and low <= value <= high


def finite(value):
    return type(value) in (int, float) and math.isfinite(value)


def relative_file(folder, name):
    require(isinstance(name, str) and name and len(name) <= 200 and
            all(part not in ("", ".", "..") for part in name.split("/")) and
            "\\" not in name and ":" not in name and not any(ord(c) < 32 for c in name),
            "expected relative bundle file")
    return folder + "/" + name


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        while True:
            data = source.read(8192)
            if not data:
                break
            digest.update(data)
            solaros.time.sleep_ms(0)
    return binascii.hexlify(digest.digest()).decode()


def validate_ports(ports, inputs):
    require(isinstance(ports, dict) and 1 <= len(ports) <= 16, "expected 1..16 ports")
    for name, port in ports.items():
        require(isinstance(name, str) and name and len(name.encode()) < 128 and "\x00" not in name,
                "invalid port name")
        fields = ("dtype", "shape", "exponents", "adapter") if inputs else ("dtype", "shape", "exponents")
        keys(port, fields, fields)
        require(port["dtype"] in DTYPE_BYTES, "invalid tensor dtype")
        shape = port["shape"]
        require(isinstance(shape, list) and 1 <= len(shape) <= 8 and
                all(integer(n, 1, TENSOR_MAX) for n in shape), "invalid fixed shape")
        size = DTYPE_BYTES[port["dtype"]]
        for n in shape:
            size *= n
        require(size <= TENSOR_MAX, "tensor exceeds runtime limit")
        exps = port["exponents"]
        require(isinstance(exps, list) and len(exps) <= TENSOR_MAX and
                all(integer(n, -2147483648, 2147483647) for n in exps), "invalid quantization")
        if port["dtype"] in ("float16", "float32", "float64", "bool"):
            require(not exps, "non-quantized tensor has exponents")
        else:
            require(exps, "quantized tensor requires exponents")
        if inputs:
            adapter = port["adapter"]
            keys(adapter, ("type", "options"), ("type",))
            require(isinstance(adapter["type"], str) and adapter["type"] in INPUT_ADAPTERS,
                    "unknown input adapter")
            options = adapter.get("options", {})
            require(isinstance(options, dict), "expected adapter options")
            if adapter["type"] == "tensor":
                require(not options, "tensor adapter has no options")
            elif adapter["type"] == "image":
                validate_image(port, options)


def validate_image(port, options):
    keys(options, ("layout", "color", "resize", "mean", "std", "pad", "x", "y", "width", "height"),
         ("layout", "color", "resize"))
    layout, color = options["layout"], options["color"]
    require(layout in ("NHWC", "NCHW", "HWC", "CHW") and color in ("RGB", "BGR", "GRAY"),
            "invalid image layout/color")
    require(options["resize"] in ("stretch", "letterbox"), "invalid image resize")
    require(port["dtype"] in ("int8", "uint8", "int16", "float32"), "unsupported image dtype")
    shape = port["shape"]
    if layout in ("NHWC", "NCHW"):
        require(len(shape) == 4 and shape[0] == 1, "image batch must be one")
        shape = shape[1:]
    else:
        require(len(shape) == 3, "image requires rank three")
    channels = 1 if color == "GRAY" else 3
    require(shape[0 if layout in ("NCHW", "CHW") else 2] == channels, "channel mismatch")
    exps = port["exponents"]
    if port["dtype"] != "float32":
        require(len(exps) in (1, channels) and all(-126 <= n <= 126 for n in exps),
                "unsupported image quantization")
    for key in ("mean", "std", "pad"):
        if key not in options:
            continue
        values = options[key]
        require(isinstance(values, list) and len(values) in (1, channels) and all(finite(v) for v in values),
                "invalid channel values")
        if key == "std":
            require(all(v > 0 for v in values), "std must be positive")
        if key == "pad":
            require(all(integer(v, 0, 255) for v in values), "pad must be bytes")
    for key in ("x", "y", "width", "height"):
        if key in options:
            require(integer(options[key], 0, 4294967295), "invalid crop")


def validate_manifest(manifest, folder):
    required = ("schema", "id", "version", "runtime", "model", "inputs", "outputs", "result")
    keys(manifest, required + ("assets", "license"), required)
    require(type(manifest["schema"]) is int and manifest["schema"] == 1, "unsupported bundle schema")
    for key in ("id", "version"):
        value = manifest[key]
        require(isinstance(value, str) and 1 <= len(value) <= 100 and
                all(c in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-" for c in value),
                "invalid bundle identity")
    runtime = manifest["runtime"]
    keys(runtime, ("backend", "version", "target"), ("backend", "version", "target"))
    require(runtime["backend"] == "espdl" and runtime["target"] == "esp32s3" and
            isinstance(runtime["version"], str), "unsupported runtime")
    model = manifest["model"]
    keys(model, ("file", "sha256"), ("file", "sha256"))
    relative_file(folder, model["file"])
    require(isinstance(model["sha256"], str) and len(model["sha256"]) == 64 and
            all(c in "0123456789abcdef" for c in model["sha256"]), "invalid model hash")
    assets = manifest.get("assets", {})
    require(isinstance(assets, dict) and len(assets) <= 32, "invalid bundle assets")
    for name, digest in assets.items():
        relative_file(folder, name)
        require(name != model["file"] and isinstance(digest, str) and len(digest) == 64 and
                all(c in "0123456789abcdef" for c in digest), "invalid asset hash")
    if "license" in manifest:
        keys(manifest["license"], ("id", "file"), ("id", "file"))
        require(isinstance(manifest["license"]["id"], str) and manifest["license"]["id"] and
                manifest["license"]["file"] in assets, "invalid license reference")
    validate_ports(manifest["inputs"], True)
    validate_ports(manifest["outputs"], False)
    result = manifest["result"]
    keys(result, ("type", "options"), ("type",))
    require(isinstance(result["type"], str) and result["type"] in RESULT_ADAPTERS,
            "unknown result adapter")
    options = result.get("options", {})
    require(isinstance(options, dict), "expected result options")
    if result["type"] == "raw":
        require(not options, "raw adapter has no options")
    elif result["type"] == "classification":
        keys(options, ("tensor", "labels", "activation", "top"), ("tensor", "labels", "activation"))
        require(options["tensor"] in manifest["outputs"], "unknown classification tensor")
        port = manifest["outputs"][options["tensor"]]
        require(port["dtype"] in ("int8", "uint8", "int16", "float32") and
                len(port["shape"]) in (1, 2) and (len(port["shape"]) == 1 or port["shape"][0] == 1),
                "classification requires one class vector")
        require(port["dtype"] == "float32" or len(port["exponents"]) == 1,
                "classification requires per-tensor quantization")
        require(options["activation"] in ("softmax", "identity"), "invalid activation")
        require(integer(options.get("top", 5), 1, min(1000, port["shape"][-1])), "invalid top count")
        relative_file(folder, options["labels"])
        require(options["labels"] in assets, "labels must be a checked bundle asset")
    elif result["type"] == "pico-detection":
        validate_pico(manifest, options)


def validate_pico(manifest, options):
    keys(options, ("input", "stages", "score_threshold", "iou_threshold", "limit", "candidate_limit", "label"),
         ("input", "stages", "label"))
    require(options["input"] in manifest["inputs"] and
            manifest["inputs"][options["input"]]["adapter"]["type"] == "image", "detector needs an image input")
    for key in ("score_threshold", "iou_threshold"):
        require(finite(options.get(key, .7 if key == "score_threshold" else .5)) and
                0 < options.get(key, .7 if key == "score_threshold" else .5) <= 1, "invalid detection threshold")
    require(integer(options.get("limit", 10), 1, 100) and
            integer(options.get("candidate_limit", 128), options.get("limit", 10), 1024), "invalid detection limits")
    require(isinstance(options["label"], str) and len(options["label"]) <= 128, "invalid detector label")
    stages = options["stages"]
    require(isinstance(stages, list) and 1 <= len(stages) <= 8, "invalid detector stages")
    used = []
    input_port = manifest["inputs"][options["input"]]
    require(input_port["adapter"]["options"]["layout"] in ("NHWC", "HWC"), "PICO expects channel-last input")
    ishape = input_port["shape"][-3:]
    for stage in stages:
        keys(stage, ("score", "bbox", "stride", "bins"), ("score", "bbox", "stride", "bins"))
        require(integer(stage["stride"], 1, 1024) and integer(stage["bins"], 2, 32), "invalid detector stage geometry")
        for key in ("score", "bbox"):
            name = stage[key]
            require(name in manifest["outputs"] and name not in used, "unknown/duplicate detector output")
            used.append(name)
        score, bbox = manifest["outputs"][stage["score"]], manifest["outputs"][stage["bbox"]]
        for port in (score, bbox):
            require(port["dtype"] == "int8" and len(port["shape"]) == 4 and port["shape"][0] == 1 and
                    len(port["exponents"]) == 1 and -126 <= port["exponents"][0] <= 126, "unsupported PICO output")
        require(score["shape"][3] == 1 and bbox["shape"][:3] == score["shape"][:3] and
                bbox["shape"][3] == 4 * stage["bins"] and
                score["shape"][1] * stage["stride"] == ishape[0] and
                score["shape"][2] * stage["stride"] == ishape[1], "detector output geometry mismatch")


def match_ports(expected, actual):
    require(set(expected) == set(actual), "model port names differ from bundle")
    for name, contract in expected.items():
        require(all(contract[key] == actual[name][key] for key in ("dtype", "shape", "exponents")),
                "model port differs from bundle: " + name)


def tensor_input(bundle, name, value, options):
    return value, None, 0


def image_input(bundle, name, value, options):
    result = solaros.inference.prepare_image(bundle.handle, name, value, options)
    return result["data"], result["transform"], result["preprocess_us"]


def tensor_values(port):
    import struct
    data, dtype = port["data"], port["dtype"]
    if dtype in ("int8", "uint8"):
        values = [v - 256 if dtype == "int8" and v >= 128 else v for v in data]
    else:
        fmt, size = ("<h", 2) if dtype == "int16" else ("<f", 4)
        values = [struct.unpack_from(fmt, data, i)[0] for i in range(0, len(data), size)]
    if dtype != "float32":
        scale = 2 ** port["exponents"][0]
        values = [v * scale for v in values]
    return values


def classification(bundle, outputs, transforms, options):
    values = tensor_values(outputs[options["tensor"]])
    require(all(math.isfinite(v) for v in values), "non-finite classification output")
    if options["activation"] == "softmax":
        maximum = max(values)
        weights = [math.exp(v - maximum) for v in values]
        total = sum(weights)
        scores = [v / total for v in weights]
    else:
        scores = values
    indices = sorted(range(len(values)), key=lambda i: values[i], reverse=True)[:options.get("top", 5)]
    return {"kind": "classification", "classes": [
        {"id": i, "label": bundle.labels[i], "score": scores[i]} for i in indices]}


def signed(value):
    return value - 256 if value >= 128 else value


def integral(data, offset, bins, scale):
    maximum = max(signed(data[offset + j]) for j in range(bins))
    weights = [math.exp((signed(data[offset + j]) - maximum) * scale) for j in range(bins)]
    return sum(j * weights[j] for j in range(bins)) / sum(weights)


def map_box(box, transform, clip=True):
    t = transform
    mapped = [int((box[0] - t["pad_left"]) * t["crop_width"] / t["resized_width"] + t["crop_x"]),
              int((box[1] - t["pad_top"]) * t["crop_height"] / t["resized_height"] + t["crop_y"]),
              int((box[2] - t["pad_left"]) * t["crop_width"] / t["resized_width"] + t["crop_x"]),
              int((box[3] - t["pad_top"]) * t["crop_height"] / t["resized_height"] + t["crop_y"])]
    if clip:
        return clip_box(mapped, t)
    return mapped


def clip_box(box, t):
    return [max(0, min(t["source_width"] - 1, box[0])), max(0, min(t["source_height"] - 1, box[1])),
            max(0, min(t["source_width"] - 1, box[2])), max(0, min(t["source_height"] - 1, box[3]))]


def iou(a, b):
    area_a = max(0, a[2] - a[0] + 1) * max(0, a[3] - a[1] + 1)
    area_b = max(0, b[2] - b[0] + 1) * max(0, b[3] - b[1] + 1)
    intersection = max(0, min(a[2], b[2]) - max(a[0], b[0]) + 1) * max(0, min(a[3], b[3]) - max(a[1], b[1]) + 1)
    union = area_a + area_b - intersection
    return intersection / union if union else 0


def pico_detection(bundle, outputs, transforms, options):
    transform = transforms[options["input"]]
    threshold, limit = options.get("score_threshold", .7), options.get("candidate_limit", 128)
    candidates, qualifying = [], 0
    for stage in options["stages"]:
        score, bbox = outputs[stage["score"]], outputs[stage["bbox"]]
        h, w = score["shape"][1:3]
        scale, box_scale = 2 ** score["exponents"][0], 2 ** bbox["exponents"][0]
        cutoff = int(math.floor(threshold * threshold / scale + .5))
        stride, bins = stage["stride"], stage["bins"]
        for index in range(h * w):
            raw = signed(score["data"][index])
            if raw <= cutoff:
                continue
            qualifying += 1
            center_x, center_y = (index % w) * stride + stride // 2, (index // w) * stride + stride // 2
            offset = index * 4 * bins
            distances = [integral(bbox["data"], offset + side * bins, bins, box_scale) * stride for side in range(4)]
            box = [center_x - distances[0], center_y - distances[1],
                   center_x + distances[2], center_y + distances[3]]
            item = (math.sqrt(raw * scale), map_box(box, transform, False))
            if len(candidates) < limit:
                candidates.append(item)
            else:
                lowest = min(range(len(candidates)), key=lambda i: candidates[i][0])
                if item[0] > candidates[lowest][0]:
                    candidates[lowest] = item
            if qualifying % 16 == 0:
                solaros.time.sleep_ms(0)
    candidates.sort(key=lambda item: item[0], reverse=True)
    kept, output_limited = [], False
    for score, box in candidates:
        if any(iou(box, other[1]) > options.get("iou_threshold", .5) for other in kept):
            continue
        if len(kept) == options.get("limit", 10):
            output_limited = True
            break
        kept.append((score, box))
    return {"kind": "detection", "coordinates": "source_pixels", "width": transform["source_width"],
            "height": transform["source_height"], "truncated": qualifying > limit or output_limited,
            "detections": [{"id": 0, "label": options["label"], "score": score,
                            "box": clip_box(box, transform)} for score, box in kept]}


def raw_result(bundle, outputs, transforms, options):
    return {"kind": "raw", "tensors": outputs}


INPUT_ADAPTERS.update({"tensor": tensor_input, "image": image_input})
RESULT_ADAPTERS.update({"raw": raw_result, "classification": classification, "pico-detection": pico_detection})


class ModelBundle:
    def __init__(self, path, mode="single", timeout=10000, native=True, load_timeout=60000):
        self.handle, self.labels = None, []
        self.native = native and hasattr(solaros.inference, "load_bundle")
        self.timeout = timeout
        require(mode in ("single", "auto", "dual"), "invalid execution mode")
        if self.native:
            # The service owns validation, preparation, decoding and model lifetime.
            if type(path) is int or (isinstance(path, str) and path and all(c in "0123456789" for c in path)):
                self.handle = int(path)
            else:
                self.handle = solaros.inference.find(path)
                if self.handle is None:
                    self.handle = solaros.inference.load_bundle(path, load_timeout)
            self.info = solaros.inference.info(self.handle)
            require(self.info["bundle"], "handle is not a model bundle")
            self.manifest = {"id": self.info["bundle_id"], "version": self.info["bundle_version"],
                             "inputs": {}, "outputs": self.info["outputs"]}
            for name, port in self.info["inputs"].items():
                contract = dict(port)
                contract["adapter"] = {"type": "image" if self.info["image_inputs"][name] else "tensor"}
                self.manifest["inputs"][name] = contract
            solaros.inference.set_mode(self.handle, mode)
            return

        folder = path.rsplit("/", 1)[0] if "/" in path else "."
        with open(path) as source:
            text = source.read(MANIFEST_MAX + 1)
        require(len(text) <= MANIFEST_MAX, "manifest exceeds 64 KiB")
        self.manifest = json.loads(text)
        del text
        validate_manifest(self.manifest, folder)
        model_path = relative_file(folder, self.manifest["model"]["file"])
        require(sha256_file(model_path) == self.manifest["model"]["sha256"], "model SHA-256 mismatch")
        for name, digest in self.manifest.get("assets", {}).items():
            require(sha256_file(relative_file(folder, name)) == digest, "asset SHA-256 mismatch: " + name)
        options = self.manifest["result"].get("options", {})
        if self.manifest["result"]["type"] == "classification":
            count = self.manifest["outputs"][options["tensor"]]["shape"][-1]
            with open(relative_file(folder, options["labels"])) as source:
                for _ in range(count + 1):
                    line = source.readline(1026)
                    if not line:
                        break
                    label = line.strip()
                    require(label and len(line) <= 1024, "invalid label")
                    self.labels.append(label)
            require(len(self.labels) == count, "label count differs from class vector")
        gc.collect()
        self.handle = solaros.inference.load(model_path, timeout)
        try:
            self.info = solaros.inference.info(self.handle)
            runtime = self.manifest["runtime"]
            require(self.info["backend"] == runtime["backend"] and self.info["target"] == runtime["target"] and
                    self.info.get("backend_version") == runtime["version"], "runtime differs from validated bundle")
            match_ports(self.manifest["inputs"], self.info["inputs"])
            match_ports(self.manifest["outputs"], self.info["outputs"])
            solaros.inference.set_mode(self.handle, mode)
        except BaseException:
            self.close()
            raise

    def run(self, inputs):
        require(self.handle is not None, "bundle is detached")
        if self.native:
            started = solaros.time.uptime_ms()
            result = solaros.inference.run_bundle(self.handle, inputs, self.timeout)
            result["call_ms"] = solaros.time.uptime_ms() - started
            return result
        require(isinstance(inputs, dict) and set(inputs) == set(self.manifest["inputs"]), "all named inputs required")
        started = solaros.time.uptime_ms()
        prepared, transforms, preprocess_us = {}, {}, 0
        for name, value in inputs.items():
            adapter = self.manifest["inputs"][name]["adapter"]
            data, transform, elapsed = INPUT_ADAPTERS[adapter["type"]](self, name, value, adapter.get("options", {}))
            prepared[name] = data
            if transform is not None:
                transforms[name] = transform
            preprocess_us += elapsed
        result = solaros.inference.run(self.handle, prepared, self.timeout)
        del prepared
        adapter = self.manifest["result"]
        begin = solaros.time.uptime_ms()
        result["result"] = RESULT_ADAPTERS[adapter["type"]](self, result["outputs"], transforms, adapter.get("options", {}))
        result["postprocess_ms"] = solaros.time.uptime_ms() - begin
        result["preprocess_us"], result["transforms"] = preprocess_us, transforms
        result["call_ms"] = solaros.time.uptime_ms() - started
        return result

    def reset(self):
        require(self.handle is not None, "bundle is closed")
        solaros.inference.reset(self.handle)

    def close(self):
        if self.handle is not None:
            solaros.inference.close(self.handle)
            self.handle = None

    def detach(self):
        """Drop this client reference; the OS model remains resident."""
        self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.detach()
