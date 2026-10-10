"""Invoke start/inspect in separate interpreters to check OS ownership.

python /dl/pipeline_client.py start SOURCE PROCESSOR MODEL LIMIT INTERVAL
python /dl/pipeline_client.py inspect ID
"""
import json
import sys
from solaros import pipeline

if sys.argv[1] == "start":
    p = pipeline.start(sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]))
    print("PIPELINE_STARTED", p)
elif sys.argv[1] == "inspect":
    p = int(sys.argv[2])
    assert p in pipeline.list()
    r = pipeline.result(p)
    s = pipeline.status(p)
    assert r is not None and r["sequence"] <= s["sequence"]
    assert pipeline.result(p, pipeline.status(p)["sequence"]) is None or not s["done"]
    print(json.dumps({"status": s, "latest": r}))
    print("PIPELINE_PYTHON_OK", p)
else:
    raise ValueError("expected start or inspect")
