#!/usr/bin/env python3
"""Generate original two-input/two-output numerical EDL2 fixtures.

Requires flatc 2.0.8 and Python flatbuffers 2.0.7; no training or model downloads.
"""
import argparse
import importlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def generate(flatc: str) -> None:
    import flatbuffers
    with tempfile.TemporaryDirectory() as generated:
        subprocess.run([flatc, "--python", "-o", generated,
                        str(ROOT / "components/espdl_schema/espdl.fbs")], check=True)
        sys.path.insert(0, generated)
        modules = {name: importlib.import_module("espdl." + name) for name in (
            "DimensionValue", "Dimension", "TensorShape", "TensorTypeAndShape",
            "TypeInfo", "ValueInfo", "Attribute", "Node", "Graph", "Model")}
        for suffix, dtype, quant, dimensions in [("int8", 3, "S8", [2, 4]),
                ("float32", 1, "F32", [2, 4]), ("simd_int8", 3, "S8", [2, 16]),
                ("optional", 3, "S8", [2, 4]),
                ("oversized", 3, "S8", [1024, 4096])]:
            b = flatbuffers.Builder(2048)

            def vector(items, int64=False):
                b.StartVector(8 if int64 else 4, len(items), 8 if int64 else 4)
                for value in reversed(items):
                    (b.PrependInt64 if int64 else b.PrependUOffsetTRelative)(value)
                return b.EndVector()

            def table(kind, fields):
                m = modules[kind]; m.Start(b)
                for field, value in fields.items(): getattr(m, "Add" + field)(b, value)
                return m.End(b)

            def port(name):
                dims = []
                for n in dimensions:
                    value = table("DimensionValue", {"DimType": 1, "DimValue": n})
                    dims.append(table("Dimension", {"Value": value}))
                shape = table("TensorShape", {"Dim": vector(dims)})
                tensor = table("TensorTypeAndShape", {"ElemType": dtype, "Shape": shape})
                info = table("TypeInfo", {"ValueType": 1, "Value": tensor})
                return table("ValueInfo", {"Name": b.CreateString(name), "ValueInfoType": info,
                                           "Exponents": vector([0], int64=True)})

            def node(operation, name, output):
                inputs = vector([b.CreateString("a"), b.CreateString("b")])
                outputs = vector([b.CreateString(output)])
                attr_name = b.CreateString("quant_type")
                quant_value = b.CreateByteVector(quant.encode())
                m = modules["Attribute"]; m.Start(b)
                m.AddName(b, attr_name); m.AddAttrType(b, 3)
                m.AddS(b, quant_value)
                attrs = vector([m.End(b)])
                return table("Node", {"Input": inputs, "Output": outputs,
                                      "Name": b.CreateString(name), "OpType": b.CreateString(operation),
                                      "Attribute": attrs})

            nodes = vector([node("Add", "sum_node", "sum"), node("Sub", "difference_node", "difference")])
            input_ports = [port("a"), port("b")]
            output_ports = [port("sum"), port("difference")]
            inputs = vector(input_ports)
            outputs = vector(output_ports)
            values = input_ports + output_ports
            if suffix == "optional":
                values.append(port(""))  # Exporter placeholder for omitted optional inputs.
            graph = table("Graph", {"Node": nodes, "Name": b.CreateString("SolarOS numerical fixture"),
                                    "Input": inputs, "Output": outputs,
                                    "Initializer": vector([]), "ValueInfo": vector(values),
                                    "TestInputsValue": vector([]), "TestOutputsValue": vector([])})
            model = table("Model", {"IrVersion": 1, "ProducerName": b.CreateString("SolarOS fixture generator"),
                                    "ModelVersion": 1, "Graph": graph})
            b.Finish(model)
            payload = bytes(b.Output())
            data = b"EDL2" + struct.pack("<III", 0, len(payload), 0) + payload
            (Path(__file__).parent / f"arithmetic_{suffix}.espdl").write_bytes(data)
            print(suffix, len(data))
        a, b = [1, -2, 3, 4, -5, 6, 7, -8], [2, 3, -4, 1, 6, -2, 0, 4]
        (Path(__file__).parent / "expected.json").write_text(json.dumps(
            {"shape": [2, 4], "a": a, "b": b, "sum": [x+y for x,y in zip(a,b)],
             "difference": [x-y for x,y in zip(a,b)]}, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(); parser.add_argument("--flatc", default="flatc")
    generate(parser.parse_args().flatc)
