+++
id = "himax-hx6538"
title = "Himax HX6538 camera"
section = "hardware"
summary = "JPEG sampling through the factory Himax HX6538 SSCMA firmware"
keywords = "himax hx6538 wiseeye camera ov5647 sscma spi jpeg sampling"
packages_any = ["driver_himax_hx6538"]
+++
# Himax HX6538 camera

The `himax-hx6538` expansion driver exposes a camera connected to a Himax
HX6538 through the existing SSCMA firmware. The HX6538 owns the sensor and
returns JPEG samples over SPI. Its firmware and installed models are preserved.

Select the `camera` package group on an ESP32-S3 with PSRAM. For a runtime
attachment, create a named SPI bus and supply its chip select:

```text
expansion attach himax-hx6538 camera0 spi=spi0 cs=21 ready=gpiox0:6 reset=gpiox0:7 power=gpiox0:11 clock=12000
camera resolutions
ramfs mount /ram 256k
camera capture /ram/sample.jpg 416x416
camera off
```

The example control lines describe SenseCAP Watcher wiring. Its board profile
attaches `camera0` automatically; runtime attachments must use available pins
and named buses on their own board.

`spi` and `cs` are required. Optional `ready` is active-high, `reset` is
active-low, and `power` is active-high. Each control accepts a native GPIO or
a named GPIO-expander line. `clock` is in kHz, defaults to 12000, and accepts
100..12000. SPI mode is 0. The bus must support transfers of at least 256 bytes.
The driver shares that bus with other devices and releases it between bounded
transport exchanges.

The first use powers and resets the chip when those controls are supplied,
then queries the firmware and sensor modes. `camera resolutions` lists the
dimensions offered by that firmware. The default is 240x240. The Watcher factory
firmware offers 240x240, 416x416, 480x480, and 640x480. Unsupported dimensions
return an error. JPEG quality is controlled by the factory firmware and is
reported as `firmware`; explicit quality overrides and orientation changes are
unsupported.

Capture requests one sample at a time. Replies and JPEG data use PSRAM, with
a 512 KiB bound on each encoded reply. The image is decoded in place and held
until the camera frame lease is released. The driver creates no background
worker. `camera off` stops sampling, disables the sensor, and frees reply
storage; the HX6538 stays powered with its firmware available.

The registered video source works with the camera service, Python/Lua camera
helpers, and the existing HTTP/RTSP publishers:

```text
job start cam-webd 416x416 5
job stop cam-webd
job start rtspd video=camera0 size=640x480 fps=5
job stop rtspd
```

Model controls and inference belong to the optional `processor-himax` native
module, included as a built-in provider by the Watcher image. See
[native image pipelines](pipelines.md) for discovery and camera-bound inference.
Ordinary sampling does not require the module. Model upload and selection are
not exposed by this version of the module.

With the `photo` package selected, `photo` opens a live viewfinder. Press Enter
to save the displayed frame, or use `photo camera0 --size 640x480` for VGA capture.

## Quick reference

Driver: `himax-hx6538`. Required: `spi=<bus> cs=<cs>`. Optional:
`ready=<gpio-or-controller:line> reset=<gpio-or-controller:line>`
`power=<gpio-or-controller:line> clock=<kHz>`. Inspect with `camera status` and
`camera resolutions`; save with `camera capture <path> [WIDTHxHEIGHT]`.
