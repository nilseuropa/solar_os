+++
id = "encoder"
title = "Encoder counts"
aliases = ["pcnt-encoder"]
section = "hardware"
summary = "Attach named quadrature encoders with explicit x1, x2, or x4 counting"
keywords = "encoder quadrature pcnt count direction x1 x2 x4 half full python lua"
packages_any = ["service_encoder"]
+++
# Encoder counts

The encoder service exposes named counters through the shell, Python, Lua,
and C. The `pcnt-encoder` expansion driver uses the ESP32 pulse-counter
peripheral to count A/B signals in hardware. Multiple encoders can run
independently, subject to available GPIOs and PCNT units.

Select the `encoder` package group for the common service and `pcnt_encoder`
for the hardware driver. Selecting the driver also includes the service.
Both are included in the `full` flavor on supported boards. The provider
supports ESP32 and ESP32-S3; it requires no Arduino or external encoder library.

## Quick reference

Use available native GPIOs for A and B. These example pins must be checked
against the board's pin table and current resource claims:

```text
expansion attach pcnt-encoder left a=1 b=2 mode=4
expansion attach pcnt-encoder right a=3 b=4 mode=2 invert=1
encoder list
encoder status left
encoder read left
encoder reset left
expansion detach left
expansion detach right
```

## Counting modes

`mode` is required at attachment. There is no implicit counting mode.

| Argument | Counting behavior |
| --- | --- |
| `mode=1` | x1: count rising edges of A; B determines direction. |
| `mode=2` | x2, half quadrature: count both edges of A; B determines direction. |
| `mode=4` | x4, full quadrature: count both edges of A and B. |

All modes require both A and B. For a complete cycle starting at A=B=0,
the sequence A rises, B rises, A falls, B falls produces a positive count.
The reverse sequence produces a negative count. `invert=1` reverses the sign.
One signal cycle produces 1, 2, or 4 counts according to the selected mode;
counts are not automatically converted into detents, revolutions, or distance.
Changing mode requires detaching and attaching again.

The current PCNT provider returns signed 32-bit accumulated counts. ESP-IDF
compensates for the 16-bit hardware counter rolling over at its configured
limits. Keep the total between -2147483648 and 2147483647; reset before
exceeding that range. Reading does not clear the count. Attachment starts
from zero; `reset` stops, clears, and restarts the counter, so edges during
that brief interval are not retained.

## Wiring and options

Connect A/B to distinct, available native GPIOs and share ground. Input
levels must be compatible with the ESP32. GPIO-expander lines cannot serve
as PCNT inputs. Both GPIOs are exclusively claimed for the attachment.

| Optional argument | Meaning and default |
| --- | --- |
| `invert=0\|1` | Reverse counting direction when 1; default 0. |
| `pull=0\|1\|2` | Internal pulls: 0 none, 1 up, 2 down; default 0. Pin support depends on the chip. |
| `filter_ns=<integer>` | Reject pulses shorter than this duration in nanoseconds; default 1000. Zero disables the filter. Values beyond hardware support are rejected. |

Choose a filter shorter than the valid signal pulses at the highest expected
speed. The hardware glitch filter does not replace suitable electrical
debouncing for mechanical contacts. The existing `rotary-encoder` driver
provides debounced knob navigation events instead of this raw count API.

Each attachment uses one PCNT unit: x1/x2 use one channel, x4 uses two channels
within that unit. Resource limits come from the chip; exhausting the pool
returns an error. Detach stops counting, releases the PCNT unit, and resets
the pins before releasing their claims. Failed detach retains the endpoint
and claims for retry; if teardown partially completed, reads can return a
not-ready error. Counting across explicit sleep is not guaranteed.

## Python and Lua

Both runtimes expose `solaros.encoder` when the common service is compiled:

| Call | Result |
| --- | --- |
| `list()` | Metadata records containing `name`, `driver`, integer `mode`, and `can_reset`. |
| `status(name)` | The same fields plus signed integer `count`. |
| `read(name)` | The signed count. |
| `reset(name)` | Set count to zero when supported; no return value. |

Names are always required. Service errors raise runtime errors. The constants
`MODE_X1`, `MODE_X2`, and `MODE_X4` equal 1, 2, and 4. All providers use this
API; a provider without reset support reports `can_reset=false` and rejects
reset requests.

Python:

```python
import solaros

solaros.expansion.attach("pcnt-encoder", "left",
                        {"a": 1, "b": 2, "mode": solaros.encoder.MODE_X4})
print(solaros.encoder.status("left"))
print(solaros.encoder.read("left"))
solaros.encoder.reset("left")
solaros.expansion.detach("left")
```

Lua:

```lua
solaros.expansion.attach("pcnt-encoder", "left", {
    a = 1, b = 2, mode = solaros.encoder.MODE_X4,
    invert = 0, pull = 0, filter_ns = 1000,
})
print(solaros.encoder.read("left"))
solaros.encoder.reset("left")
solaros.expansion.detach("left")
```

An encoder measures motion independently of the [motor service](motor.md).
Attaching one does not enable automatic speed or position control.

## C providers

Consumers include `solar_os_encoder.h` and use `solar_os_encoder_get()`,
`solar_os_encoder_status()`, `solar_os_encoder_read()`, and
`solar_os_encoder_reset()`. Providers register a name, driver, counting mode,
context, and read callback. Reset and shutdown callbacks are optional.
Callbacks are serialized per endpoint outside the registry lock. In-flight
operations prevent unregister; failed shutdown keeps the registration for
retry. The provider owns its context until unregister succeeds.
