from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MacModuleTest(unittest.TestCase):
    def test_cpu_display_suspend_and_failure_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = build(Path(directory))
            subprocess.run([str(binary)], check=True, timeout=30)


def build(directory, trace=False):
    source = ROOT / "modules/mac/main"
    core = [source / "vendor" / name for name in (
        "via.c", "scc.c", "iwm.c", "mouse.c", "rtc.c", "ncr.c", "sony/disc.c",
        "musashi/m68kcpu.c", "musashi/m68kopac.c", "musashi/m68kopdm.c",
        "musashi/m68kopnz.c", "musashi/m68kops_pre.c")]
    sources = [(p, ["-include", str(source / "port.h")]) for p in core]
    sources += [(source / name, []) for name in ("machine.c", "video.c", "input.c", "disk.c", "floppy.c")]
    sources += [(source / "mac.c", ["-Dmain=solar_os_mac_main"]),
                (ROOT / "tests/host/mac_module_test.c", ["-DSOLAR_OS_MAC_PORT_IMPLEMENTATION"])]
    objects = []
    for i, (path, options) in enumerate(sources):
        obj = directory / f"{i}.o"
        subprocess.run(["cc", "-std=gnu11", "-O2", "-g", "-fno-strict-aliasing",
                        "-ffunction-sections", "-fdata-sections", "-Wno-unused-variable",
                        "-I", str(ROOT / "include"), "-I", str(source),
                        "-I", str(source / "vendor"), "-I", str(source / "vendor/musashi"),
                        *(["-DSOLAR_OS_MAC_TRACE"] if trace else []),
                        *options, "-c", str(path), "-o", str(obj)], check=True)
        objects.append(str(obj))
    binary = directory / "mac-test"
    subprocess.run(["cc", *objects, "-Wl,--gc-sections", "-o", str(binary)], check=True)
    return binary
