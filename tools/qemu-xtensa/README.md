# SolarOS QEMU tools

Targets with native input, host storage, or audio build this pinned Espressif revision.
Headless targets with host storage and audio disabled use the binary in `tools.json`:

```text
7411393a2f61e6f0b05af00276d30780769e0839
esp-develop-9.2.2-20261109
```

`scripts/solaros_qemu_build.py` applies `solar-input.patch`, copies the devices
under `overlay/`, and shares `include/solar_os_qemu_*_protocol.h` with the
guest firmware. It builds only the ESP32-S3 machine and caches the source,
build tree, and installed binary under PlatformIO's `tool-solaros-qemu` package.
The cache stamp covers the source pin, overlay, protocol, build profile, and
builder. Changing any of these rebuilds the extension.

The `solar-os-input` device is a separate memory-mapped peripheral at
`0x600ff000`. Keyboard events carry canonical USB HID usages, modifier bits,
and down/up state. Touch events carry normalized absolute coordinates; mouse
events carry signed relative deltas. Pointer records include buttons and wheel
deltas. Reading the type register latches the whole record until the guest
writes POP. Generation changes and explicit release-all records mark focus
loss, queue overflow, and reset. The firmware retries short periods of queue
pressure and resets the input queue after a sustained stall.

The SDL focus callback releases the frontend's tracked keys and notifies the
input device. Both touch and relative mouse handlers are registered with QEMU,
so standard `info mice` / `mouse_set` monitor commands select the pointer mode.
The `focused` QOM property uses the same device callback for automated tests.

The `solar-os-hostfs` device at `0x600f8000` exposes a selected host directory.
Its `root` property is configured by the launcher; without it the device reports
not ready. A synchronous command register, four arguments, portable errno codes,
and a 4096-byte transfer buffer implement file and directory operations. File
metadata and seek offsets use explicitly encoded little-endian 64-bit values.
Host descriptors are private to the device and closed on reset. Each path is
resolved relative to the root descriptor without following symlinks. The guest
registers an ESP-IDF VFS at `/host` and includes it in the storage mount registry.
The `solar-os-audio` device at `0x600fa000` exposes 48 kHz stereo S16LE playback
through QEMU's audio API. A 4096-frame FIFO and 1024-frame staging area provide
bounded writes with backpressure. STOP drains the FIFO, mixer, and host transfer
buffer before making the stream available again; RESET aborts playback. The
guest registers an exclusive stream and an output/volume device with the audio
service. The launcher selects SDL, WAV capture, silent output, or disables audio.
No capture endpoint is registered. The shared ABI defines frame counts and
little-endian stereo samples. A small `AUD_is_empty_out` query also includes
QEMU's pending host transfer buffer so closing a stream preserves its tail.

The `qemu` board capability excludes all these packages from physical targets.

Run the firmware codec tests with:

```sh
make -C tests/host qemu_input_test
tests/host/qemu_input_test
```

Run the emulator device tests against a built extension with:

```sh
SOLAR_OS_QEMU_INPUT_TEST_BINARY=/path/to/qemu-system-xtensa \
  python3 -m pytest -q tests/test_qemu_input_device.py tests/test_qemu_hostfs.py tests/test_qemu_serial.py tests/test_qemu_audio.py
```

These use QMP input injection and the qtest MMIO interface to test the real
device, including modifiers, touch, relative movement, focus reset, overflow,
consistency of a latched record across a reset, and captured PCM including volume,
mute, queue backpressure, and stream tails. They need no firmware or
graphical desktop. Source builds require the host dependencies listed in
`doc/manual/qemu.md`; on Windows use WSL or supply a built extension through
`custom_qemu_executable`.
