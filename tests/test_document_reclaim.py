from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DocumentReclaimTest(unittest.TestCase):
    def test_suspended_document_caches_release_and_rebuild_without_losing_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            for app in ("reader", "writer"):
                source = (ROOT / f"src/apps/solar_os_{app}.c").read_text()
                self.assertIn(f".state_release_cleanup = {app}_detach_reclaimer", source)
                names = (f"{app}_reclaim_layout", f"{app}_detach_reclaimer",
                         f"{app}_suspend", f"{app}_resume", f"{app}_stop")
                Path(tmp, f"{app}_cache.inc").write_text(production_functions(source, names))
            binary = Path(tmp, "document-cache")
            subprocess.run(["cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-function", "-Wno-unused-variable", "-ffunction-sections", "-fdata-sections",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            "-include", str(ROOT / "tests/host/compat.h"),
                            "-I", tmp, "-I", str(ROOT / "tests/host"),
                            "-I", str(ROOT / "src"), "-I", str(ROOT / "src/services"),
                            str(ROOT / "tests/host/document_cache_test.c"),
                            str(ROOT / "tests/host/doc_test_support.c"),
                            str(ROOT / "src/services/solar_os_doc.c"),
                            str(ROOT / "src/services/solar_os_paged_array.c"),
                            "-Wl,--gc-sections", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
