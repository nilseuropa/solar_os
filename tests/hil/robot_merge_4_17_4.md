# Robot integration into dev/4.17.4

Merge inputs: `dev/4.17.4` at `0f7c03a9` and `module/robot` at `ad67863f`.
The incoming work adds the feedback controller service/job, optional robot
joint library, native motion ABI, motor/encoder claims, timestamped scalar
samples, shell commands, Python/Lua bindings and manuals.

## Conflict resolution

- Keep the review branch's immediate parameter retirement, generation-checked
  callback references, normalized-access snapshots and mutex initialization.
- Apply writer claims when acquiring those same callback references. Ordinary
  and normalized setters cannot bypass claims. Range updates require a live,
  unclaimed provider with no callbacks in flight.
- Keep one reference count; discard the incoming duplicate access counter.
  Retirement hides paths and invalidates claims immediately, reserves names
  until callbacks finish, and permits the owner to retry unregister safely.
- Retain Synth/Funcgen teardown waits, the split HTTP/storage bindings and the
  recursive binding-source reader. Include both new modules in descriptor
  checks: 65 modules and 941 exports.
- Extend controller/parameter tests to cover retirement during an owned
  callback and safe provider replacement, and include robot cleanup in the VM
  teardown checks.

## Validation

- 252 combined regression tests passed, including the existing review checks,
  controller/robot bindings, native execution scopes and MicroPython checks.
- Eight C executables passed: controller math, controller service, parameter
  concurrency, robot joints, robot integration, motor service, encoder service
  and stream registry. These include claims, calibration, polarity, PPR,
  deadlines, command expiry, provider replacement and cleanup retry.
- Manual generation passed with 287 topics, including controllers and robot.
- Memory contracts passed: 285 packages, 1,268 source files and 943 API sites,
  with zero errors.
- Firmware builds passed for `solar_term`, `esp32_s3_devkitc1_n16r8`,
  `odroid_go` and `seeed_reterminal_sticky`.
- Robot library build and relocation/import verification passed against the
  merged SolarTerm SDK: 26,524 bytes; imports `memcpy`, `memset` and
  `solar_os_native_library_host_v1`. SHA-256:
  `77ccb49dfa96db15922eaca4611cfaa71b9bf1a01f8cccc8e753a23746ad079e`.
- `git diff --check`: passed.

The version stays `4.17.3`. No device flashing or physical robot actuation is
part of this merge validation. The hardware evidence in the earlier review
record applies to its pre-merge firmware, not this merged image.

Raw validation output is retained under `/tmp/solar_os_4174_robot_*`.
