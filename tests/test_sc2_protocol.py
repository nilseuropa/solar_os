"""SC2 portable core (protocol/sc2): sanitizer runs, strict warnings,
C++ header compatibility, SDK independence and vector freshness."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / "protocol/sc2"
ED = ROOT / "src/vendor/meshcore/ed25519"
WG = ROOT / "components/wireguard_lwip/src"
HOST = ROOT / "tests/host"
VENDOR = [ED / "fe.c", ED / "ge.c", ED / "sc.c", ED / "sha512.c",
          WG / "crypto/refc/x25519.c", WG / "crypto/refc/chacha20.c",
          WG / "crypto/refc/chacha20poly1305.c", WG / "crypto/refc/poly1305-donna.c", WG / "crypto.c"]
INCLUDES = ["-I" + str(LIB / "include"), "-I" + str(LIB / "vectors"), "-I" + str(ED),
            "-I" + str(WG / "crypto/refc"), "-I" + str(WG), "-idirafter", str(ROOT / "include")]
SANITIZE = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g", "-O1"]


def sources():
    return sorted(str(p) for p in (LIB / "src").glob("*.c"))


class Sc2ProtocolTest(unittest.TestCase):
    def test_suites_under_sanitizers(self):
        with tempfile.TemporaryDirectory() as directory:
            objects = []
            for src in VENDOR:
                obj = Path(directory) / (src.stem + ".o")
                # orlp ref10 code left-shifts negative values (implementation-defined in practice),
                # so shift checks are disabled for the vendor objects only.
                subprocess.run(["cc", "-std=c11", "-w", *SANITIZE, "-fno-sanitize=shift", *INCLUDES,
                                "-c", str(src), "-o", str(obj)], check=True)
                objects.append(str(obj))
            for name in ("crypto", "session", "frame", "admin", "engine", "client"):
                binary = Path(directory) / name
                extra = []
                if name in ("engine", "client"):
                    extra = [str(ROOT / "src/services" / f"solar_os_sc2_{part}.c")
                             for part in ("engine", "selftest")]
                if name == "client":
                    extra.append(str(ROOT / "src/services/solar_os_sc2_client.c"))
                subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *SANITIZE, *INCLUDES,
                                "-I" + str(ROOT / "src/services"),
                                str(HOST / f"sc2_{name}_test.c"), *extra, *sources(), *objects, "-o", str(binary)],
                               check=True)
                subprocess.run([str(binary)], check=True, stdout=subprocess.DEVNULL)

    def test_strict_warnings(self):
        with tempfile.TemporaryDirectory() as directory:
            for src in sources():
                subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-Wshadow",
                                "-Wconversion", "-Wno-sign-conversion", "-Wstrict-prototypes",
                                "-Wmissing-prototypes", "-Wvla", *INCLUDES, "-c", src,
                                "-o", str(Path(directory) / "x.o")], check=True)

    @unittest.skipUnless(shutil.which("c++"), "no C++ compiler")
    def test_headers_compile_as_cxx(self):
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / "h.cpp"
            src.write_text('#include "sc2.h"\nint main() { return sc2_strerror(SC2_OK)[0] == 0; }\n')
            subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                            "-I" + str(LIB / "include"), str(src)], check=True)

    def test_no_sdk_or_core_includes(self):
        allowed_vendor = {"crypto/refc/chacha20poly1305.h", "ge.h", "sc.h", "sha512.h", "crypto/refc/x25519.h", "crypto.h"}
        for path in list((LIB / "src").glob("*.c")) + list((LIB / "include").glob("*.h")):
            text = path.read_text()
            # mbedTLS headers only inside the opt-in backend block
            text = re.sub(r"#if defined\(SC2_USE_MBEDTLS\).*?#else", "", text, flags=re.S)
            for inc in re.findall(r'#include\s+[<"]([^>"]+)[>"]', text):
                ok = (inc.startswith("sc2") or inc in allowed_vendor or
                      inc in {"stdint.h", "stddef.h", "stdbool.h", "string.h", "stdlib.h", "limits.h"})
                self.assertTrue(ok, f"{path.name} includes {inc}")

    def test_vectors_up_to_date(self):
        python = os.environ.get("SC2_VECTOR_PYTHON", sys.executable)
        probe = subprocess.run([python, "-c", "import nacl"], capture_output=True)
        if probe.returncode != 0:
            self.skipTest("PyNaCl not installed (set SC2_VECTOR_PYTHON to a venv python)")
        subprocess.run([python, str(LIB / "tools/gen_vectors.py"), "--check"], check=True)


if __name__ == "__main__":
    unittest.main()
