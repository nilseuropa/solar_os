from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MemoryArenaTest(unittest.TestCase):
    def test_lifetimes_coalescing_and_pointer_stability(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp, "arena")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I" + str(ROOT / "src/services"),
                str(ROOT / "tests/host/memory_arena_test.c"),
                str(ROOT / "src/services/solar_os_memory_arena.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
