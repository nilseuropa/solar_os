from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class C64ModuleTest(unittest.TestCase):
    def test_core_media_input_audio_and_foreground_lifecycle(self):
        source = ROOT / "modules/c64/main"
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "c64-test"
            rom_source = Path(directory) / "roms_data.c"
            subprocess.run(["python3", str(source.parent / "scripts/embed_roms.py"),
                            str(source.parent / "rom"), str(rom_source)], check=True)
            subprocess.run([
                "c++", "-std=c++17", "-O1", "-g", "-fno-strict-aliasing",
                "-fno-exceptions", "-fno-rtti", "-fno-omit-frame-pointer",
                "-fsanitize=address,undefined", "-no-pie",
                "-I", str(ROOT / "include"), "-I", str(source),
                str(ROOT / "tests/host/c64_module_test.cpp"), str(source / "input.cpp"),
                str(source / "audio.cpp"),
                *map(str, sorted((source / "vendor").glob("*.cpp"))),
                str(rom_source),
                "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=90, env={
                **os.environ, "ASAN_OPTIONS": "detect_leaks=1",
                "UBSAN_OPTIONS": "halt_on_error=1"})


if __name__ == "__main__":
    unittest.main()
