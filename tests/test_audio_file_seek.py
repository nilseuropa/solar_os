from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cc") and shutil.which("ffmpeg"), "requires cc and ffmpeg")
class AudioFileSeekTest(unittest.TestCase):
    def test_production_playback_positions_and_pcm(self):
        source = (ROOT / "src/services/solar_os_audio.c").read_text()
        functions = [
            "audio_volume_arg_valid", "audio_get_u16le", "audio_get_u32le",
            "audio_wav_should_cancel", "audio_wav_progress_interval_ms",
            "audio_wav_report_progress", "audio_wav_read_exact", "audio_wav_read_info_from_file",
            "audio_mp3_synchsafe_u32", "audio_mp3_seek_payload", "audio_mp3_fill_input",
            "audio_mp3_consume_input", "audio_mp3_fill_output_info",
            "audio_mp3_probe_from_file",
            "audio_mp3_playback_flush", "audio_mp3_playback_append",
            "audio_player_convert_block", "audio_play_wav_stream", "audio_mp3_advance_time",
            "audio_mp3_seek_frame", "audio_play_mp3_stream",
        ]
        extracted = []
        for name in functions:
            match = re.search(r"^static [^\n]*\b" + name + r"\([\s\S]*?^}", source, re.M)
            self.assertIsNotNone(match, name)
            extracted.append(match.group())
        for name in ("solar_os_audio_file_probe", "solar_os_audio_play_wav",
                     "solar_os_audio_play_mp3", "solar_os_audio_file_play"):
            match = re.search(r"^esp_err_t " + name + r"\([\s\S]*?^}", source, re.M)
            self.assertIsNotNone(match, name)
            extracted.append(match.group())
        with tempfile.TemporaryDirectory(prefix="solaros-audio-seek-") as directory:
            tmp = Path(directory)
            (tmp / "audio_file_playback.inc").write_text("\n\n".join(extracted))
            fixtures = []
            for name, codec, channels, extra in [
                ("mono.wav", "pcm_s16le", 1, []),
                ("stereo.wav", "pcm_u8", 2, []),
                ("cbr.mp3", "libmp3lame", 2, ["-b:a", "128k"]),
                ("vbr.mp3", "libmp3lame", 1, ["-q:a", "3"]),
                ("mpeg2.mp3", "libmp3lame", 2, ["-ar", "22050", "-b:a", "8k"]),
            ]:
                path = tmp / name
                subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                    "-i", "sine=frequency=431:sample_rate=44100:duration=" + ("60" if name.endswith(".mp3") else "3"), "-c:a", codec,
                    "-ac", str(channels), *extra, str(path)], check=True)
                fixtures.append(path)
            binary = tmp / "audio_file_seek_test"
            subprocess.run(["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-DSOLAR_OS_AUDIO_CODEC_HOST_TEST", "-DSOLAR_OS_PACKAGE_SERVICE_AUDIO=1",
                "-DSOLAR_OS_PACKAGE_SERVICE_AUDIO_CODECS=1", "-fno-pie", "-no-pie",
                f"-I{tmp}", f"-I{ROOT / 'tests/host'}", f"-I{ROOT / 'include'}",
                f"-I{ROOT / 'src/services'}", f"-I{ROOT / 'src'}",
                f"-I{ROOT / 'components/minimp3/include'}",
                str(ROOT / "tests/host/audio_file_seek_test.c"),
                str(ROOT / "src/services/solar_os_native_audio.c"),
                str(ROOT / "src/services/solar_os_native_handles.c"),
                str(ROOT / "src/services/solar_os_audio_codec.c"),
                str(ROOT / "src/services/solar_os_audio_pcm.c"),
                str(ROOT / "components/minimp3/minimp3_impl.c"), "-Wl,--wrap=mp3dec_decode_frame",
                "-lm", "-o", str(binary)], check=True)
            subprocess.run([str(binary), *map(str, fixtures)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
