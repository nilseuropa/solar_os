# Himax model bundle contract

`schemas/solaros-himax-model-bundle.schema.json` defines schema-two bundles for
the HX6538 SSCMA provider. Existing schema-one ESP-DL bundles remain unchanged.
The shared identity, hashed files, assets and license envelope is retained;
camera input and SSCMA outputs replace the ESP-DL tensor adapters.

`runtime.version` is the SSCMA protocol contract `sscma-v0`, independent of the
accelerator firmware's dated build version. Optional `runtime.firmware` bounds
declare minimum/maximum builds. Missing bounds leave firmware compatibility
unknown. The provider descriptor applies equally to built-in and external
`processor-himax` implementations and requires capture, model selection and
upload capabilities. The optional Himax provider implements model management;
the driver retains transport and camera ownership. See
[Himax models](manual/himax-models.md) for programming and selection.

Input width/height describe the neural network's RGB input, not a sensor capture
mode. The accelerator firmware supplies image preparation and model-specific
postprocessing. Outputs preserve SSCMA coordinates and scores. Result types map
to `boxes`, `classes` or `points`; the checked labels asset has exactly
`class_count` nonempty UTF-8 lines in class-ID order.

The payload is an unmodified Vela TFLite artifact with an `ethos-u` operator.
Host structural checks do not prove operator support, numerical accuracy,
firmware compatibility or fit in a physical model slot. Deployment uses an
explicit slot validated against the device layout; flash addresses are not
bundle fields. Storage installation, device programming and model selection
remain separate operations.

Bundle/catalog validation and distribution tooling live in `solar_os_zoo`.
The schema is published here for the runtime-owned provider contract. Host
validation does not replace a successful upload and inference test on the
specific device, firmware and provider version.
