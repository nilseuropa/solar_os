"""Host lifecycle coverage; set SOLAR_OS_DOOM_TEST_WAD for gameplay coverage.

No copyrighted game data is bundled with the module or tests.
"""
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "modules/doom/main"


class DoomModuleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.stage = Path(cls.temporary.name)
        objects = []
        sources = sorted((SOURCE / "vendor/prboom").glob("*.c"))
        sources = [p for p in sources if p.name != "lprintf.c"]
        sources += sorted((SOURCE / "vendor/tables").glob("*.c"))
        sources += [SOURCE / p for p in ("doom.c", "port.c", "video.c", "i_main.c", "i_sound.c", "sound_mixer.c")]
        # Use the host C library for pure string/math functions. The target
        # builds private locale-free helpers in libc.c.
        sources += [ROOT / "tests/host/doom_module_test.c"]
        for i, path in enumerate(sources):
            obj = cls.stage / f"{i}.o"
            options = []
            if path.name == "doom.c": options += ["-Dmain=solar_os_doom_main"]
            if path.name == "port.c": options += ["-DSOLAR_OS_DOOM_PORT_IMPLEMENTATION"]
            if path.name == "d_deh.c": options += ["-DProcessDehFile=solar_os_doom_unused_deh"]
            if path.name != "doom_module_test.c": options += ["-include", str(SOURCE / "port.h")]
            subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-fno-strict-aliasing",
                            "-DHAVE_CONFIG_H", "-ffunction-sections", "-fdata-sections", "-w",
                            "-I", str(ROOT / "include"), "-I", str(SOURCE),
                            "-I", str(SOURCE / "vendor/prboom/include"),
                            "-I", str(SOURCE / "vendor/tables/include"), *options,
                            "-c", str(path), "-o", str(obj)], check=True)
            objects.append(str(obj))
        cls.binary = cls.stage / "doom-test"
        subprocess.run(["cc", *objects, "-Wl,--gc-sections", "-lm", "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_module(self, path, mode, *args):
        subprocess.run([str(self.binary), str(path), mode, *map(str, args)], check=True, timeout=30)

    def test_missing_and_invalid_wads(self):
        self.run_module(self.stage / "missing.wad", "error")
        invalid = [b"", b"IWAD" + struct.pack("<II", 0xffffffff, 12),
                   b"IWAD" + struct.pack("<II", 1, 12) + struct.pack("<II8s", 29, 1, b"E1M1")]
        # The final entry sits beyond the first directory-read batch.
        count = 257
        invalid.append(b"IWAD" + struct.pack("<II", count, 12) +
                       struct.pack("<II8s", 0, 0, b"EMPTY") * (count - 1) +
                       struct.pack("<II8s", 12 + count * 16, 1, b"BAD"))
        for i, data in enumerate(invalid):
            path = self.stage / f"invalid-{i}.wad"
            path.write_bytes(data)
            self.run_module(path, "error")

    def test_engine_failure_cleans_up(self):
        path = self.stage / "empty-map.wad"
        path.write_bytes(b"IWAD" + struct.pack("<II", 1, 12) + struct.pack("<II8s", 0, 0, b"E1M1"))
        self.run_module(path, "error")
        for fail_after in (1, 2, 4, 8):
            self.run_module(path, "error", fail_after)

    def test_sample_mixer(self):
        binary = self.stage / "sound-mixer"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(SOURCE), str(SOURCE / "sound_mixer.c"),
                        str(ROOT / "tests/host/doom_sound_mixer_test.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

    @unittest.skipUnless(os.environ.get("SOLAR_OS_DOOM_TEST_WAD"), "set SOLAR_OS_DOOM_TEST_WAD for real game data")
    def test_game_input_suspend_stop_and_failures(self):
        path = Path(os.environ["SOLAR_OS_DOOM_TEST_WAD"])
        # Exercise IWADs without PrBoom's additional fonts and translations.
        data = path.read_bytes()
        _, count, directory = struct.unpack_from("<4sII", data)
        has_effects = any(data[directory+i*16+8:directory+i*16+10] == b"DS" for i in range(count))
        self.run_module(path, "game" if has_effects else "no-samples")
        if has_effects:
            self.run_module(path, "exchange")
        entries = []
        for i in range(count):
            entry = data[directory + i * 16:directory + (i + 1) * 16]
            name = entry[8:].rstrip(b"\0")
            if not name.startswith((b"CR", b"DIG", b"STBR", b"BOX")) and name != b"M_SETUP":
                entries.append(entry)
        plain = self.stage / "plain.wad"
        plain.write_bytes(b"IWAD" + struct.pack("<II", len(entries), directory) + data[12:directory] + b"".join(entries))
        self.run_module(plain, "game" if has_effects else "no-samples")
        self.run_module(path, "audio-denied")
        self.run_module(path, "synth-denied")
        self.run_module(path, "no-synth")
        if has_effects:
            self.run_module(path, "switch-rate")
        # Invalid DMX sample counts must be ignored by the audio backend.
        bad_samples = bytearray(data)
        for i in range(count):
            position, size, name = struct.unpack_from("<II8s", data, directory + i * 16)
            if name.startswith(b"DS") and size >= 8:
                struct.pack_into("<I", bad_samples, position + 4, size + 1)
        invalid_audio = self.stage / "invalid-audio.wad"
        invalid_audio.write_bytes(bad_samples)
        self.run_module(invalid_audio, "silent-samples")
        self.run_module(path, "short")
        for fail_after in (1, 4, 32, 128, 512):
            self.run_module(path, "error", fail_after)
