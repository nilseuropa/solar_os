from pathlib import Path
import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "modules/aogmaneo"


class AOgmaNeoTest(unittest.TestCase):
    def test_packaged_sdk_header_and_c_example(self):
        spec = importlib.util.spec_from_file_location("aogmaneo_sdk", ROOT / "scripts/pack_native_sdk.py")
        packer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(packer)
        module_id, version, headers = packer.load_headers(MODULE / "module.toml")
        self.assertEqual((module_id, version), ("aogmaneo", "1.0.0"))
        self.assertEqual([name for name, _, _ in headers], ["aogmaneo/solar_os_aogmaneo_abi.h"])
        with tempfile.TemporaryDirectory(prefix="solaros-aogmaneo-sdk-") as temporary:
            root = Path(temporary)
            for name, data, _ in headers:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-I{root}", f"-I{ROOT / 'include'}", "-c", str(MODULE / "examples/predict.c"),
                "-o", str(root / "predict.o")], check=True)

    def test_provider_learning_lifecycle_and_allocation_failures(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("g++ unavailable")
        with tempfile.TemporaryDirectory(prefix="solaros-aogmaneo-") as temporary:
            binary = Path(temporary) / "aogmaneo"
            subprocess.run([compiler, "-std=c++17", "-O1", "-g", "-DUSE_STD_MATH=1",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                f"-I{ROOT / 'include'}", f"-I{MODULE}",
                str(ROOT / "tests/host/aogmaneo_library_test.cpp"),
                str(MODULE / "engine.cpp"), str(MODULE / "library.cpp"),
                *map(str, sorted((MODULE / "vendor/aogmaneo").glob("*.cpp"))),
                "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})


if __name__ == "__main__":
    unittest.main()
