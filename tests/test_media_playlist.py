from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MediaPlaylistTest(unittest.TestCase):
    def test_webradio_binary_and_nvs_migration_and_m3u_precedence(self):
        with tempfile.TemporaryDirectory(prefix="solaros-radio-m3u-") as directory:
            binary = Path(directory) / "webradio_catalog_test"
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                f"-I{ROOT / 'tests/host'}", f"-I{ROOT / 'src'}", f"-I{ROOT / 'src/services'}",
                str(ROOT / "tests/host/webradio_catalog_test.c"),
                str(ROOT / "src/services/solar_os_webradio_catalog.c"),
                str(ROOT / "src/services/solar_os_m3u.c"), "-o", str(binary),
            ], check=True)
            for mode in ([], ["binary"], ["m3u"]):
                subprocess.run([str(binary), *mode], check=True)

    def test_parser_migration_and_transactional_import(self):
        with tempfile.TemporaryDirectory(prefix="solaros-m3u-test-") as directory:
            binary = Path(directory) / "media_playlist_test"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-D_POSIX_C_SOURCE=200809L", "-include", str(ROOT / "tests/host/compat.h"),
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                f"-I{ROOT / 'tests/host'}", f"-I{ROOT / 'src'}", f"-I{ROOT / 'src/services'}",
                str(ROOT / "tests/host/media_playlist_test.c"),
                str(ROOT / "src/services/solar_os_m3u.c"),
                str(ROOT / "src/services/solar_os_media_playlist.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
