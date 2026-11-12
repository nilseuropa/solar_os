#!/usr/bin/env python3
"""Compile ABI probes, check loader compatibility, and reject unsupported C."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from build import HERE, ROOT, CACHE, run


def main():
    pin = json.loads((HERE / "upstream.json").read_text())
    source = CACHE / "source" / ("tinycc-" + pin["revision"])
    if not (CACHE / "solar-tcc").exists():
        raise SystemExit("run build.py first")
    includes = [HERE / "sdk", source / "include", ROOT / "include",
                ROOT / "modules/examples/hello-library/main"]
    sys.path.insert(0, str(ROOT / "modules/espdl"))
    from verify import verify
    report = {"revision": pin["revision"], "production_validator_pass": True, "artifacts": {}, "rejected": []}
    probes = {name: HERE / "examples" / (name + ".c") for name in ("library_probe", "scalar_probe")}
    probes.update({name: ROOT / "modules/examples" / name / "main" /
                   (name.replace("-", "_") + ".c") for name in ("hello-library", "library-user")})
    for name, path in probes.items():
        obj, elf = CACHE / (name + ".o"), CACHE / (name + ".elf")
        run(CACHE / "solar-tcc", path, obj, *includes)
        run(CACHE / "module", obj, elf, "main")
        run(CACHE / "elf-check", elf)
        imports = verify(elf, {"solar_os_native_host_v1", "solar_os_native_job_host_v1",
                               "solar_os_native_library_host_v1"})
        report["artifacts"][name] = {"bytes": elf.stat().st_size, "imports": imports,
                                     "sha256": hashlib.sha256(elf.read_bytes()).hexdigest()}
    rejected = {
        "float": "float f(float x) { return x+1; }",
        "integer64": "int f(int x) { long long v=x; v=v*1234567890123LL; return v; }",
        "vla": "int f(int n) { int a[n]; a[n-1]=1; return a[n-1]; }",
        "variadic": "int f(int n, ...) { return n; }",
        "aggregate": "struct A { int n; }; struct A f(void) { struct A a={1}; return a; }",
        "asm": 'int f(void) { asm("nop"); return 0; }',
        "syntax": "int f(void) { return missing + ; }",
    }
    negative = CACHE / "negative"; negative.mkdir(exist_ok=True)
    for name, text in rejected.items():
        path = negative / (name + ".c"); path.write_text(text)
        result = subprocess.run([str(CACHE / "solar-tcc"), str(path), str(path.with_suffix(".o")),
                                 *map(str, includes)], capture_output=True, text=True, timeout=15)
        if result.returncode <= 0 or "error:" not in result.stderr:
            raise RuntimeError(f"unsupported case did not fail cleanly: {name}: {result.stderr}")
        report["rejected"].append(name)
    (CACHE / "checks.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
