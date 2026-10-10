# Upload alongside fixtures to /inference-hil; run with agent script python.
from solaros import inference
import struct
import gc

a = [1, -2, 3, 4, -5, 6, 7, -8]
b = [2, 3, -4, 1, 6, -2, 0, 4]
expected = {"sum": [x + y for x, y in zip(a, b)],
            "difference": [x - y for x, y in zip(a, b)]}

def fails(fn, *args):
    try:
        fn(*args)
    except Exception:
        return
    raise AssertionError("expected error")

assert not inference.list(), "run this fixture with no pre-existing resident models"
print("PYTHON_INFERENCE_START")
fails(inference.load, "/inference-hil/arithmetic_oversized.espdl")
print("OVERSIZED_LOAD_REJECTED")
for name, dtype, fmt, width in [("arithmetic_int8", "int8", "<8b", 4),
                         ("arithmetic_float32", "float32", "<8f", 4),
                         ("arithmetic_simd_int8", "int8", "<32b", 16)]:
    path = "/inference-hil/" + name + ".espdl"
    print("LOADING", name)
    handle = inference.load(path)
    info = inference.info(handle)
    print("MODEL", name, info)
    assert info["inputs"]["a"]["shape"] == [2, width]
    assert info["outputs"]["sum"]["dtype"] == dtype
    repeats = width // 4
    av, bv = struct.pack(fmt, *(a * repeats)), struct.pack(fmt, *(b * repeats))
    typed = {"data": av, "dtype": dtype, "shape": [2, width]}
    for i in range(75):
        r = inference.run(handle, {"b": bv, "a": typed if i % 2 else av})
        for port in expected:
            assert list(struct.unpack(fmt, r["outputs"][port]["data"])) == expected[port] * repeats
        if i % 10 == 0:
            gc.collect()
    print("TIMING", name, r["input_us"], r["inference_us"], r["output_us"], r["elapsed_us"])
    fails(inference.run, handle, {"a": av})
    fails(inference.run, handle, {"a": av, "b": bv[:-1]})
    fails(inference.run, handle, {"unknown": av, "b": bv})
    fails(inference.run, handle, {"a\x00suffix": av, "b": bv})
    typed["dtype"] = "uint8"
    fails(inference.run, handle, {"a": typed, "b": bv})
    typed["dtype"] = dtype
    typed["dtype"] = dtype + "\x00suffix"
    fails(inference.run, handle, {"a": typed, "b": bv})
    typed["dtype"] = dtype
    typed["shape"] = [1, width * 2]
    fails(inference.run, handle, {"a": typed, "b": bv})
    fails(inference.run, handle, {"a": av, "b": bv}, 0)
    fails(inference.run, handle, {"a": av, "b": bv}, 60001)
    fails(inference.run, handle, {"a": av, "b": bv}, 1)
    # Recovery must also fit below the former 100 ms worker-reap delay.
    recovered = inference.run(handle, {"a": av, "b": bv}, 50)
    assert list(struct.unpack(fmt, recovered["outputs"]["sum"]["data"])) == expected["sum"] * repeats
    fails(inference.load, "/inference-hil/missing.espdl")
    fails(inference.load, "/inference-hil/corrupt.espdl")
    retained = r["outputs"]["sum"]["data"]
    inference.reset(handle)
    inference.close(handle)
    assert list(struct.unpack(fmt, retained)) == expected["sum"] * repeats
    fails(inference.info, handle)
    again = inference.load(path)
    assert again != handle
    inference.close_all()
    fails(inference.info, again)
    handles = [inference.load(path) for _ in range(4)]
    fails(inference.load, path)
    inference.close_all()
    print("PYTHON_MODEL_OK", name)
for _ in range(4):
    inference.load("/inference-hil/arithmetic_int8.espdl")
print("RESIDENT_HANDLES", inference.list())
print("PYTHON_INFERENCE_OK")  # These four models remain resident; explicitly unload them before another fixture run.
