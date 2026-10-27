from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AmigaModuleTest(unittest.TestCase):
    def test_cpu_chipset_media_input_audio_and_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            source = ROOT / "modules/amiga/main"
            sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            objects = []
            files = list((source / "vendor").glob("*.cpp"))
            files += [source / "rtc.cpp", source / "keyboard.cpp"]
            files += [source / "vendor/musashi" / name for name in ("m68kcpu.c", "m68kops.c")]
            files += [ROOT / "tests/host/amiga_module_test.cpp"]
            for index, path in enumerate(files):
                obj = Path(directory) / f"{index}.o"
                vendor = path.is_relative_to(source)
                subprocess.run([
                    "cc" if path.suffix == ".c" else "c++", "-O1", "-g", "-fno-strict-aliasing",
                    "-ffunction-sections", "-fdata-sections", "-DSOLAR_OS_AMIGA=1",
                    *sanitize, "-I", str(ROOT / "include"), "-I", str(source),
                    "-I", str(source / "vendor"), "-I", str(source / "vendor/musashi"),
                    *(["-include", str(source / "port.h")] if vendor else []),
                    "-c", str(path), "-o", str(obj)], check=True)
                objects.append(str(obj))
            binary = Path(directory) / "amiga-test"
            subprocess.run(["c++", *sanitize, *objects, "-Wl,--gc-sections", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=45,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1", "UBSAN_OPTIONS": "halt_on_error=1"})


if __name__ == "__main__":
    unittest.main()
