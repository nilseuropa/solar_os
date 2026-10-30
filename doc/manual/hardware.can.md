+++
id = "hardware.can"
title = "Classic CAN"
section = "hardware"
summary = "Attach a TWAI CAN controller, exchange frames, and inspect bus errors"
aliases = ["twai", "twai-can"]
keywords = "can twai transceiver standard extended remote frame bus-off recovery"
packages_any = ["can_twai"]
+++
# Classic CAN

The `twai-can` expansion driver exposes one ESP32 or ESP32-S3 classic CAN
controller. Connect TX and RX to a compatible external CAN transceiver.
The transceiver provides the CANH/CANL electrical interface; ESP32 pins do not
connect directly to those bus wires. Use the same bitrate on all nodes and
terminate the bus at its two ends.

```text
expansion attach twai-can can0 tx=16 rx=17 bitrate=500 listen=0
can list
can status can0
can send can0 0x123 01020304
can recv can0 100
expansion detach can0
```

Choose available pins on your board. Bitrates are 25, 50, 100, 125, 250, 500,
800, or 1000 kbit/s. Default `listen=1` receives without acknowledging or
transmitting. Set `listen=0` for normal bus participation. Optional
`select=<gpio-or-controller:line>` controls a transceiver enable or signal
multiplexer; `active=0|1` selects its enabled level (default 1). Detaching
releases selection and all resource claims.

In the `io` app, press `n`, select `can`, and set the controller name, TX/RX
GPIOs, bitrate, and mode. An optional Select GPIO and Active level control a
transceiver enable pin. CAN controllers appear in the Buses view, where runtime
controllers have `Autostart` and `Remove CAN bus` actions. The CAN option is
available when the `twai-can` driver is installed and supported by the board.
Use `expansion attach` for selection through an external GPIO controller.

Frames carry up to eight data bytes. IDs default to standard 11-bit form;
add `ext` for a 29-bit ID. Hex data contains two digits per byte, without
spaces; `-` sends a zero-length frame. Add `rtr` for a remote request; its DLC
is the length of the supplied hex argument, and its data bytes are ignored.
The command reports when a frame is queued. Check controller status for
transmission failures; queue admission is not a delivery acknowledgment.
CAN FD and application protocols such as CANopen are not provided.

`can recv` waits up to 1000 ms, with zero as the default. The C service API
also bounds send and receive waits to 1000 ms. Operations and detach are
serialized. A receive timeout returns without producing a frame.

After bus-off, `can recover can0` starts the controller's bus-free recovery.
Poll `can status can0` until its state becomes `stopped`, then run
`can start can0`. Detach is refused while recovery is in progress.

## Quick reference

`can [list]` lists the attached controller. `can status <name>` shows its
state, queue depths, error counters, failed transmissions, and missed receives.
`can send <name> <id> <hex-or-> [ext] [rtr]` queues a frame.
`can recv <name> [timeout-ms]` reads one frame. `can recover <name>` begins
bus-off recovery; `can start <name>` restarts a stopped controller.
