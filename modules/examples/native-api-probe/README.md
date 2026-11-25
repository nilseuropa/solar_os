# Native API probe

This synchronous native app uses public SDK headers and the existing host entry
point. It has no firmware board/flavor configuration or private OS includes.

In an ESP-IDF environment with the ESP32-S3 tools installed:

```sh
idf.py -DIDF_TARGET=esp32s3 elf
```

The normal `project_elf` target produces `solaros_native_api_probe.app.elf`.
Run it through `load` on compatible firmware. It checks service discovery,
ABI/size rejection, optional providers, and memory allocation/release, then
prints `NATIVE_API_PROBE_OK`. The `system` service is required; older firmware
without it is intentionally refused.

See the [native SDK guide](../../../doc/manual/native-modules.md#use-the-c-sdk)
for service lookup and resource ownership.
