+++
id = "motor"
title = "Motor control"
aliases = ["drv8830"]
section = "hardware"
summary = "Control named motors with normalized drive commands"
keywords = "motor normalized drive python lua provider drv8830 voltage coast brake fault int i2c"
packages_any = ["service_motor"]
+++
# Motor control

The motor service exposes named motor endpoints through the shell, Python,
Lua, and C. Drivers register endpoints with this service, so applications use
the same API across controllers. The current hardware backend is DRV8830.

Select the `motor` package group for the common service and the `drv8830`
group for that controller. Selecting a driver also includes its required
service. Both groups are included in the `full` flavor where supported.
`motor list` reports each endpoint's driver and drive, brake, and clear
capabilities. Optional operations return an unsupported-operation error when
the provider does not implement them.

## Quick reference

```text
expansion attach drv8830 motor0 i2c=i2c0 addr=0x60 max_mv=3000
motor list
motor status motor0
motor drive motor0 0.5
motor drive motor0 -0.5
motor coast motor0
motor brake motor0
motor clear motor0
expansion detach motor0
```

## Normalized control

`drive` takes a finite value from `-1.0` through `1.0`. The sign selects
direction; the magnitude requests a fraction of the backend's configured
full-scale output. Zero coasts. This value is a control demand, not a measured
speed, torque, voltage, or universal PWM duty cycle. Each driver maps it to
its hardware and reports the quantized applied command when available.

Values outside the range, NaN, and infinity are rejected. `brake` requests
active braking when supported. `coast` releases the bridge output.
`clear` is an explicit fault-recovery operation that leaves the motor
coasting. Commands remain active after the shell or API call returns.
Neither coast nor brake switches off the motor supply.

## Python and Lua

Both runtimes expose `solaros.motor` when the motor service is compiled:

- `list()`: return motor metadata records.
- `status(name)`: read one named motor's state and diagnostics.
- `drive(name, value)`: apply a normalized signed command.
- `coast(name)`: release the motor output.
- `brake(name)`: request active braking, when supported.
- `clear(name)`: coast and clear driver-supported fault latches.

Names are required, including when only one motor is present. Successful
control calls return no value; service errors raise runtime errors.

Metadata records contain `name`, `driver`, `can_drive`, `can_brake`, and
`can_clear`. Status records also contain:

| Field | Meaning |
| --- | --- |
| `mode` | `unknown`, `coast`, `forward`, `reverse`, or `brake`. |
| `drive_valid`, `drive` | Whether the applied normalized command is known, and its value. This is not measured shaft motion. |
| `faults` | Normalized present or recorded fault conditions. |
| `blocking_faults` | Conditions that currently prevent drive. Recorded history can remain after recovery. |
| `fault_input_present`, `fault_input_asserted` | Availability and sampled assertion state of an optional hardware fault input. |
| `voltage_valid`, `voltage_mv` | Optional nominal voltage-setting diagnostic, in millivolts. This is not a measured output voltage. |
| `raw_valid`, `raw_control`, `raw_fault` | Optional driver-specific register diagnostics. Their bit layout is not part of the common motor contract. |

Both modules export normalized fault-mask constants `FAULT_UNKNOWN`,
`FAULT_OVERCURRENT`, `FAULT_UNDERVOLTAGE`, `FAULT_OVERTEMPERATURE`, and
`FAULT_CURRENT_LIMIT`. Test these against `faults` or `blocking_faults`;
do not interpret driver register bits as common fault masks.

Python, after attachment:

```python
import solaros

print(solaros.motor.list())
try:
    solaros.motor.drive("motor0", 0.5)
    print(solaros.motor.status("motor0"))
finally:
    solaros.motor.coast("motor0")
```

Lua, after attachment:

```lua
local ok, err = pcall(function()
    solaros.motor.drive("motor0", -0.5)
    local state = solaros.motor.status("motor0")
    print(state.mode, state.drive, state.blocking_faults)
end)
solaros.motor.coast("motor0")
if not ok then error(err) end
```

## DRV8830 backend

One controller provides one motor endpoint. Multiple controllers can share
a named I2C bus at distinct seven-bit addresses, `0x60` through `0x68`.
Connect SDA/SCL to that bus, share ground, and supply the controller and motor
according to their ratings. The bus must run at 400 kHz or less.

Optional `max_mv=482..5060` sets the backend's full-scale voltage limit;
the default is 5060 mV. It is rounded down to a supported VSET code.
A normalized magnitude of one selects that code. Other magnitudes select
the nearest fraction of that code. Codes below the minimum valid VSET code
6 map to coast. With the default full scale, commands whose magnitude is
below approximately 0.0873 coast; the first nonzero applied command is
`6/63`, approximately `0.0952`. Quantization and this deadband are
backend-specific.

The chip internally regulates the average voltage across the motor winding.
If the target exceeds the available supply, it saturates at full duty.
SolarOS reads the programmed setting, not a motor or supply-voltage
measurement. Current limiting is configured by the external sense resistor.

FAULTn is optional. Add `int=<available-native-gpio>` when wired; this is
an active-low, open-drain signal. Use a pull-up compatible with the ESP32
input; the driver also enables the native input pull-up. SolarOS claims
the address and optional GPIO; the named bus owns SDA/SCL. The fault register
also works without the interrupt wire. The input is sampled on status and
drive calls; there is no asynchronous callback.

Python and Lua attachment dictionaries/tables accept `i2c`, `addr`,
optional integer `int`, and optional integer `max_mv`.
For example, Python can attach with:

```python
solaros.expansion.attach("drv8830", "motor0",
                        {"i2c": "i2c0", "addr": 0x60, "max_mv": 3000})
```

Attach and successful detach coast the bridge. Reversal passes through
standby. Active FAULT, a latched overcurrent or extended-current-limit
condition, or an asserted fault input blocks drive. Recovered undervoltage
and overtemperature history remains visible without requiring clear.
A failed control write attempts to coast. A communication failure cannot
guarantee a physical stop. Failed detach retains the service registration
and expansion resource claims for retry.

Electrical behavior and register diagnostics follow the
[TI DRV8830 datasheet](https://www.ti.com/lit/ds/symlink/drv8830.pdf).

## C providers and consumers

Applications include `solar_os_motor.h` and use
`solar_os_motor_get()`, `solar_os_motor_status()`,
`solar_os_motor_drive()`, `solar_os_motor_coast()`,
`solar_os_motor_brake()`, and `solar_os_motor_clear()`.

Drivers register one endpoint per motor channel using
`solar_os_motor_register()`. Status and coast callbacks are required;
normalized drive, brake, clear, and shutdown callbacks are optional.
Callbacks are serialized per endpoint and execute outside the registry
lock. Providers own their hardware mapping, initial coast state, and fault
policy. Successful unregister stops the motor before allowing the driver
to release its context. In-flight operations and failed shutdown prevent
unregister and preserve the endpoint for retry.
