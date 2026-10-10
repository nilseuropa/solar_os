# Run load, run, then unload in separate interpreter sessions.
# python /dl/inference_residency.py load|run|unload [/dl/models/arithmetic/bundle.json]
import sys
from solaros import inference

path = sys.argv[2] if len(sys.argv)>2 else "/dl/models/arithmetic/bundle.json"
action = sys.argv[1]
handle = inference.find(path)
if action == "load":
    assert handle is None, "fixture already resident; explicitly unload it first"
    handle = inference.load_bundle(path, 60000)
elif action == "run":
    assert handle is not None, "load fixture in a preceding interpreter session"
    result = inference.run_bundle(handle, {
        "a": bytes([1,254,3,4,251,6,7,248]),
        "b": bytes([2,3,252,1,6,254,0,4]),
    })
    assert result["outputs"]["sum"]["data"] == bytes([3,1,255,5,1,4,7,252])
    assert result["result"]["kind"] == "raw"
elif action == "unload":
    assert handle is not None
    inference.unload(handle)
    assert inference.find(path) is None
    assert handle not in inference.list()
else:
    raise ValueError("expected load, run, or unload")
if action != "unload":
    assert handle in inference.list()
    assert inference.info(handle)["bundle"]
print("INFERENCE_RESIDENCY_PYTHON_OK", action, handle)
