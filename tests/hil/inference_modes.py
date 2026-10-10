# Run on SolarOS with an integer model and one prepared input tensor.
# python /inference_modes.py /model.espdl /input.bin [cycles] [repeats]
from solaros import inference, time
import gc
import binascii
import hashlib
import json
import sys


def checksum(result):
    digest = hashlib.sha256()
    for name in sorted(result["outputs"]):
        digest.update(name.encode())
        digest.update(result["outputs"][name]["data"])
    return binascii.hexlify(digest.digest()).decode()


def summarize(values):
    values = sorted(values)
    count = len(values)
    return {"min": values[0], "median": (values[(count - 1) // 2] + values[count // 2]) / 2,
            "max": values[-1], "mean": sum(values) / count}


def rejected(fn, *args):
    try:
        fn(*args)
    except (OSError, ValueError):
        return
    raise AssertionError("expected rejection")


cycles = int(sys.argv[3]) if len(sys.argv) > 3 else 3
repeats = int(sys.argv[4]) if len(sys.argv) > 4 else 5
assert 1 <= cycles <= 20 and 1 <= repeats <= 100
with open(sys.argv[2], "rb") as source:
    data = source.read()
reference = None
for cycle in range(cycles):
    gc.collect()
    begin = time.uptime_ms()
    handle = inference.load(sys.argv[1])
    load_ms = time.uptime_ms() - begin
    try:
        inputs = inference.inputs(handle)
        assert len(inputs) == 1
        input_name = next(iter(inputs))
        assert inputs[input_name]["bytes"] == len(data)
        assert inference.info(handle)["mode"] == "single"
        rejected(inference.set_mode, handle, "invalid")
        rejected(inference.set_mode, handle, "dual\x00suffix")
        modes = ("single", "auto", "dual") if cycle % 2 == 0 else ("dual", "single", "auto")
        for mode in modes:
            inference.set_mode(handle, mode)
            assert inference.info(handle)["mode"] == mode
            timings, calls = [], []
            for run in range(repeats + 1):
                gc.collect()
                started = time.uptime_ms()
                result = inference.run(handle, {input_name: data})
                elapsed = time.uptime_ms() - started
                actual = checksum(result)
                if reference is None:
                    reference = actual
                assert actual == reference, (cycle, mode, actual, reference)
                if run:
                    timings.append(result["inference_us"] / 1000)
                    calls.append(elapsed)
                del result
            print(json.dumps({"cycle": cycle, "mode": mode, "repeats": repeats,
                              "load_ms": load_ms, "checksum": reference,
                              "native_ms": summarize(timings), "call_ms": summarize(calls)}))
        # Deadline failure must join parallel work and leave the model reusable.
        inference.set_mode(handle, "dual")
        rejected(inference.run, handle, {input_name: data}, 1)
        inference.reset(handle)
        result = inference.run(handle, {input_name: data})
        assert checksum(result) == reference
        retained = result["outputs"]
        del result
    finally:
        inference.close(handle)
    rejected(inference.set_mode, handle, "single")
    assert retained
    del retained
    print("MODEL_RELEASED", cycle)
gc.collect()
print("INFERENCE_MODES_OK", reference, gc.mem_free())
