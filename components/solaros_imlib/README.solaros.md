# SolarOS imlib subset

Source: https://github.com/espressif/esp-vision/tree/ecda8004b95e1df275ac2a1f9cdf487d03defc63/components/imlib

The MIT-licensed OpenMV kernels and Espressif portable CMSIS compatibility
headers are pinned to that revision. Each source retains its copyright and
license notice. Only statistics, histogram, binary operations, mean/median/
Gaussian filtering, morphology, image difference, and blobs are exposed.
Gaussian uses the upstream convolution kernel with Pascal coefficients.

SolarOS adaptations:

- `imlib.h` calls the native allocation/progress port; it has no MicroPython
  dependency. `core.c` contains only the required helpers from `imlib.c`.
- Binary threshold writeback supports unscaled binary/grayscale/RGB565 images
  without importing the upstream general drawing/codec subsystem.
- Threshold/histogram scans and blob flood fills poll native progress.
- Statistics clamp negative floating-point variance roundoff to zero before
  taking its square root.
- Exhausted bounded blob flood-fill storage fails instead of returning partial
  components. Filters retain upstream edge handling and numerical behavior.
- Configuration disables file I/O, codecs, other detectors, and GPU hooks.

The native service owns one admitted request at a time. Its heap-backed frame
allocator and linked-list allocations use request-owned PSRAM, with joined
worker teardown after cancellation or allocation failure. No framebuffer arena
is reserved while idle. The constant LAB lookup table remains in firmware flash.
