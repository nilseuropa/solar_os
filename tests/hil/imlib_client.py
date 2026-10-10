"""Run with PNG fixtures installed by imlib_subset.py; owns all opened images."""
import json
from solaros import image, vision

owned = []


def open_image(name):
    handle = image.open("/dl/.imlib-" + name + ".png")
    owned.append(handle)
    return handle


def pixel(handle, x, y):
    w, h = image.size(handle)
    data = image.to_rgb(handle)
    return data[(y * w + x) * 3]


def close(handle):
    image.close(handle)
    owned.remove(handle)


def must_fail(fn, *args):
    try:
        fn(*args)
    except (ValueError, TypeError, OSError):
        return
    raise AssertionError("invalid arguments accepted")


try:
    source = open_image("scene")
    reference = open_image("reference")
    color = open_image("color")
    noise = open_image("noise")
    original = image.to_rgb(source)
    s = vision.statistics(source, {"x": 8, "y": 6, "width": 8, "height": 6})
    assert s["channels"]["gray"]["mean"] == 200
    assert s["channels"]["gray"]["stdev"] == 0
    h = vision.histogram(source)
    assert len(h["channels"]["gray"]) == 256
    assert abs(sum(h["channels"]["gray"]) - 1) < 0.0001
    o = {"thresholds": [[128, 255]]}
    blobs = vision.blobs(source, o)
    assert len(blobs["blobs"]) == 1
    b = blobs["blobs"][0]
    assert (b["x"], b["y"], b["width"], b["height"], b["pixels"]) == (8, 6, 8, 6, 48)
    assert abs(b["cx"] - 11.5) < 0.01
    red = vision.blobs(color, {"format": "rgb565", "thresholds": [[20, 80, 30, 127, 0, 127]]})
    assert len(red["blobs"]) == 1 and red["blobs"][0]["pixels"] == 48
    color_hist = vision.histogram(color, {"format": "rgb565"})
    assert len(color_hist["channels"]["l"]) == 101
    assert len(color_hist["channels"]["a"]) == 256
    mask = vision.binary(source, o)
    owned.append(mask)
    assert pixel(mask, 8, 6) == 255 and pixel(mask, 0, 0) == 0
    metrics = []
    for operation in ("erode", "dilate", "opening", "closing"):
        r = vision.process(mask, operation)
        owned.append(r["image"])
        assert pixel(r["image"], 10, 8) == 255
        if operation == "opening":
            assert pixel(r["image"], 2, 2) == 0
        if operation == "erode":
            assert pixel(r["image"], 8, 6) == 0
        metrics.append({"operation": operation, "process_us": r["process_us"],
                        "workspace_peak_bytes": r["workspace_peak_bytes"]})
        close(r["image"])
    close(mask)
    for operation, lower, upper in (("mean", 27, 29), ("gaussian", 63, 64), ("median", 0, 0)):
        r = vision.process(noise, operation)
        owned.append(r["image"])
        assert lower <= pixel(r["image"], 4, 4) <= upper
        metrics.append({"operation": operation, "process_us": r["process_us"],
                        "workspace_peak_bytes": r["workspace_peak_bytes"]})
        close(r["image"])
    inv = vision.invert(source)
    owned.append(inv)
    assert pixel(inv, 8, 6) == 55
    close(inv)
    diff = vision.difference(source, reference)
    owned.append(diff)
    assert pixel(diff, 8, 6) == 180 and pixel(diff, 0, 0) == 0
    close(diff)
    roi = vision.blobs(source, {"x": 4, "y": 2, "width": 20, "height": 16,
                                "output_width": 10, "output_height": 8, "thresholds": [[128, 255]]})
    assert roi["blobs"][0]["x"] == 8 and roi["blobs"][0]["pixels"] == 12
    merged = vision.blobs(source, {"thresholds": [[128, 255]], "area_threshold": 1,
                                  "pixels_threshold": 1, "merge": True, "margin": 8})
    assert len(merged["blobs"]) == 1 and merged["blobs"][0]["pixels"] == 49
    assert image.to_rgb(source) == original
    must_fail(vision.mean, source, {"ksize": 4})
    must_fail(vision.binary, source)
    must_fail(vision.statistics, source, {"bins": 1})
    must_fail(vision.statistics, source, {"width": -1})
    must_fail(vision.statistics, source, {"output_widht": 32})
    must_fail(vision.blobs, source, {"thresholds": [[255, 128]]})
    must_fail(vision.blobs, source, {"thresholds": [[0, 255]], "merge": 1})
    must_fail(vision.process, source, "mean", {}, reference)
    for _ in range(20):
        r = vision.process(source, "gaussian")
        owned.append(r["image"])
        close(r["image"])
        assert len(vision.blobs(source, o)["blobs"]) == 1
    print(json.dumps({"event": "imlib", "client": "python", "blobs": blobs["blobs"],
                      "color_blobs": red["blobs"], "metrics": metrics}))
    print("IMLIB_PYTHON_OK")
finally:
    for handle in reversed(owned):
        image.close(handle)
