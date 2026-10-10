# Original numerical model fixtures

These small, original single-model EDL2 files have two named inputs (`a`, `b`),
shape `[2, 4]`, and two named outputs (`sum`, `difference`). Their graph contains
Add and Sub operators. One fixture uses int8 with exponent 0; the other float32.
`expected.json` gives reference values. They test generic inference without
image preprocessing, camera ownership, or a task-specific model adapter.

`arithmetic_simd_int8.espdl` widens the int8 ports to `[2,16]`, exercising the
ESP32-S3 vector Add/Sub path with four repetitions of the reference values.

`arithmetic_optional.espdl` includes an unnamed internal value-info placeholder,
as used by compatible exporters for omitted optional operator inputs. The schema
suite accepts it while continuing to reject unnamed public input/output ports.

`arithmetic_oversized.espdl` has the same int8 graph with `[1024,4096]` ports;
its workspace exceeds an 8 MiB PSRAM board. Use it to check load failure and
subsequent recovery, rather than running numerical inference.

Regenerate with FlatBuffers compiler 2.0.8 and Python flatbuffers 2.0.7:

```sh
python3 tests/fixtures/inference/generate.py
```

No fixtures are embedded in firmware. Copy the models and `device.py` /
`device.lua` to a mounted `/inference-hil` volume. Also create `corrupt.espdl`
containing invalid bytes. Run `python /inference-hil/device.py`
and `lua /inference-hil/device.lua`. The full suite can exceed the agent script
runner's fixed 30-second deadline. Each program checks all three numerical models
across 75 calls, descriptor/input validation, load failures, timeout recovery,
explicit release, stale handles, the four-model limit, and interpreter teardown
with resident models. Start each fixture with an empty OS model registry; the
fixture checks this before loading or calling global `close_all`. It prints
the four remaining handles; explicitly unload them before the other interpreter
fixture or another run. The success markers are
`PYTHON_INFERENCE_OK` and `LUA_INFERENCE_OK`.
