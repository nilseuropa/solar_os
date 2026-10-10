# Runtime bundle contract fixtures

These manifests exercise the firmware's raw, classification, and PICO adapter
contracts. They are test inputs, not a model catalog or publication source.
`raw.json` references the original numerical fixture in
`tests/fixtures/inference/arithmetic_int8.espdl` and is also used by the native
ELF hardware acceptance harness.

Model releases and their bundle descriptions are maintained in `solar_os_zoo`.
Firmware tests keep these independent fixtures so they need no zoo checkout.
