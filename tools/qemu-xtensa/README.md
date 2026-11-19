# SolarOS QEMU tools

The headless target uses the official binary release described by `tools.json`.
The display target builds the same pinned Espressif revision with native input:

```text
7411393a2f61e6f0b05af00276d30780769e0839
esp-develop-9.2.2-20261109
```

`scripts/solaros_qemu_build.py` applies `solar-input.patch`, copies the device
under `overlay/`, and shares `include/solar_os_qemu_input_protocol.h` with the
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

Run the firmware codec tests with:

```sh
make -C tests/host qemu_input_test
tests/host/qemu_input_test
```

Run the emulator device tests against a built extension with:

```sh
SOLAR_OS_QEMU_INPUT_TEST_BINARY=/path/to/qemu-system-xtensa \
  python3 -m pytest -q tests/test_qemu_input_device.py
```

These use QMP input injection and the qtest MMIO interface to test the real
device, including modifiers, touch, relative movement, focus reset, overflow,
and consistency of a latched record across a reset. They need no firmware or
graphical desktop. Source builds require the host dependencies listed in
`doc/manual/qemu.md`; on Windows use WSL or supply a built extension through
`custom_qemu_executable`.
