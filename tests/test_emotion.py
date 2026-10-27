from pathlib import Path
import subprocess
import re
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class EmotionTest(unittest.TestCase):
    def test_lua_objects_validation_and_cleanup(self):
        with tempfile.TemporaryDirectory() as tmp:
            for source, output in (("src/services/solar_os_emotion.h", "emotion_runtime_header.inc"),
                                   ("src/services/solar_os_emotion.c", "emotion_runtime.inc")):
                Path(tmp, output).write_text(re.sub(r'^#(?:include|pragma).*\n', '',
                    (ROOT / source).read_text(), flags=re.M))
            lua = ROOT / "components/lua/lua/src"
            Path(tmp, "emotion_visibility.inc").write_text(production_functions(
                (ROOT / "src/apps/solar_os_lua.c").read_text(), ("solua_emotion_visibility",)))
            sources = sorted(str(p) for p in lua.glob("*.c") if p.name not in ("lua.c", "luac.c"))
            binary = Path(tmp, "lua")
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror",
                "-DSOLAR_OS_PACKAGE_SERVICE_AUDIO=1", "-I", tmp,
                "-I", str(ROOT / "include"), "-I", str(ROOT / "src/apps"), "-I", str(lua),
                str(ROOT / "modules/emotion/main/emotion.c"),
                str(ROOT / "tests/host/emotion_lua_test.c"), *sources,
                "-lm", "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_runtime_ownership_queue_and_teardown(self):
        with tempfile.TemporaryDirectory() as tmp:
            for source, output in (("src/services/solar_os_emotion.h", "emotion_runtime_header.inc"),
                                   ("src/services/solar_os_emotion.c", "emotion_runtime.inc")):
                text = (ROOT / source).read_text()
                Path(tmp, output).write_text(re.sub(r'^#(?:include|pragma).*\n', '', text, flags=re.M))
            for audio in (0, 1):
                binary = Path(tmp, "runtime")
                subprocess.run([
                    "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    f"-DSOLAR_OS_PACKAGE_SERVICE_AUDIO={audio}",
                    "-I", tmp, "-I", str(ROOT / "include"),
                    str(ROOT / "modules/emotion/main/emotion.c"),
                    str(ROOT / "tests/host/emotion_runtime_test.c"), "-o", str(binary),
                ], check=True)
                subprocess.run([str(binary)], check=True)

    def test_geometry_timing_instances_and_sound(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp, "emotion")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(ROOT / "include"),
                str(ROOT / "modules/emotion/main/emotion.c"),
                str(ROOT / "tests/host/emotion_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
