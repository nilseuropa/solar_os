# quirc native QR backend

Source: https://github.com/dlbeer/quirc
Pinned revision: `927d680904dc95fdff4cd9d022eb374b438ff8f2`.
License: ISC; retained in LICENSE and each upstream source file.

ESP-VISION/OpenMV's QR recognizer also derives from quirc. SolarOS uses the
native upstream library, avoiding their MicroPython-specific imlib allocators,
image objects, filesystem, and interpreter error handling.

Local adaptations:

- ESP-IDF build with single-precision math, PSRAM-only allocation, and no
  interpreter dependency.
- Optional progress callback during recognition for cooperative yield and
  cancellation; per-recognizer state, never global allocator state.
- Skip zero-length copy from a null initial image in quirc_resize.

No other imlib algorithms or GPL-conditioned code are included.
