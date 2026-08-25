# Native Continuous-Parameter Contract

See the [stream, control, parameter, and OSC binding model](binding-model.md)
for the complete routing graph, value conversions, and runtime ownership.

Native foreground applications can expose tunable continuous parameters
through `solar_os_parameters.h`. The registry is independent of any physical
input. ADC potentiometers, script-provided controls, MIDI-oriented controls,
and future input devices all target the same stable parameter paths.

An application registers one owner and an array of
`solar_os_parameter_definition_t` while it is available. Each definition has a
relative name, label, unit, minimum, maximum, quantization step, curve, getter,
setter, and callback context. The public path is `<owner>.<name>`; for example,
owner `synth` plus name `filter.cutoff` publishes
`synth.filter.cutoff`.

```c
static esp_err_t cutoff_get(void *user, float *value);
static esp_err_t cutoff_set(void *user, float value);

static const solar_os_parameter_definition_t parameters[] = {
    {
        .name = "filter.cutoff",
        .label = "Filter cutoff",
        .unit = "Hz",
        .minimum = 40.0f,
        .maximum = 18000.0f,
        .step = 1.0f,
        .curve = SOLAR_OS_PARAMETER_CURVE_LOGARITHMIC,
        .get = cutoff_get,
        .set = cutoff_set,
        .user = NULL,
    },
};

static solar_os_parameter_registration_t registration =
    SOLAR_OS_PARAMETER_REGISTRATION_INIT;

solar_os_parameters_register("synth", parameters, 1, &registration);
```

The service validates range and finite values before calling the setter and
quantizes to `step` relative to `minimum`. A logarithmic curve requires a
positive minimum. Normalized access uses unsigned 16-bit values and applies the
declared curve in the registry; app callbacks always receive native units.

Register parameters only while callbacks and their context are valid. A
resumable foreground app should unregister during suspend and stop, then
register again after resume restores its state:

```c
solar_os_parameters_unregister(&registration);
```

Bindings are not deleted when a parameter disappears. The Controls job keeps
the latest input value and retries the stable path when the application
registers it again. This makes app suspend/resume and preset loading compatible
with physical controls. Use pickup for hardware knobs when a parameter can
change independently; the binding waits for the physical value to meet or
cross the current app value before taking control.

Setters must be bounded and non-blocking. They run from normal SolarOS task
context, never from an ISR or application render callback. A setter should apply
the complete parameter update and return its `esp_err_t`; the registry reports
the error to the binding instead of converting it to a shell command.
