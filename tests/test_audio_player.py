from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AudioPlayerTest(unittest.TestCase):
    def test_concurrent_ring_eof_destroy_and_partial_construction(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory, 'player.c')
            source.write_text((ROOT / 'src/services/solar_os_audio_player.c').read_text())
            binary = Path(directory, 'player')
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
                '-pthread', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-include', str(ROOT / 'tests/host/compat.h'),
                '-I' + str(ROOT / 'tests/host/audio_player_stubs'),
                '-I' + str(ROOT / 'tests/host'), '-I' + str(ROOT / 'src'),
                '-I' + str(ROOT / 'src/services'),
                str(source), str(ROOT / 'tests/host/audio_player_test.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
