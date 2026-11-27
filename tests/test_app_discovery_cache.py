from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AppDiscoveryCacheTest(unittest.TestCase):
    def test_cached_enumeration_refresh_and_failure_recovery(self):
        source = (ROOT / "src/apps/solar_os_app_registry.c").read_text()
        start = source.index("#if SOLAR_OS_NATIVE_FOREGROUND_ENABLED\ntypedef struct")
        end = source.index("const solar_os_app_registry_entry_t *solar_os_app_registry_find_by_app", start)
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "app_discovery.inc").write_text(source[start:end])
            binary = Path(tmp, "discovery")
            for enabled, foreground in ((1, 1), (0, 0), (0, 1)):
                with self.subTest(native_modules=enabled, foreground=foreground):
                    subprocess.run([
                        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-Wno-unused-variable",
                        f"-DSOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES={enabled}",
                        f"-DSOLAR_OS_NATIVE_FOREGROUND_ENABLED={foreground}",
                        "-I", tmp, str(ROOT / "tests/host/app_discovery_cache_test.c"),
                        "-o", str(binary),
                    ], check=True)
                    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
