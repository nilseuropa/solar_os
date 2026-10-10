# Run on SolarOS: python /path/to/image_rgb.py /path/to/small-picture.png
from solaros import image
import gc
import sys


def raises(kind, fn, *args):
    try:
        fn(*args)
    except kind:
        return
    raise AssertionError("expected " + kind.__name__)


handle = image.open(sys.argv[1])
try:
    width, height = image.size(handle)
    full = image.to_rgb(handle)
    assert isinstance(full, bytearray)
    assert len(full) == width * height * 3
    for out_width, out_height in [(2, 1), (7, 5)]:
        resized = image.to_rgb(handle, out_width, out_height)
        for y in range(out_height):
            for x in range(out_width):
                source = ((y * height // out_height) * width + x * width // out_width) * 3
                target = (y * out_width + x) * 3
                assert resized[target:target + 3] == full[source:source + 3]
    owned = bytes(resized)
    del full
    gc.collect()
    raises(ValueError, image.to_rgb, handle, 2)
    raises(ValueError, image.to_rgb, handle, 0, 1)
    raises(ValueError, image.to_rgb, handle, 2048, 2048)
    # 1.92 MB fits the native pixel limit but exceeds the 512 KiB Python heap.
    raises(MemoryError, image.to_rgb, handle, 800, 800)
    assert image.to_rgb(handle, 7, 5) == resized
finally:
    image.close(handle)
assert resized == owned
raises(ValueError, image.to_rgb, handle)
gc.collect()
print("RGB_PYTHON_OK", gc.mem_free())
