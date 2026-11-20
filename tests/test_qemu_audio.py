"""Test QEMU's PCM queue and actual WAV backend without guest firmware."""
import array
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time
import unittest
import wave

ROOT = Path(__file__).resolve().parents[1]
ABI = {name: int(value, 0) for name, value in re.findall(
    r"#define SOLAR_OS_QEMU_AUDIO_(\w+) (0x[0-9a-f]+|[0-9]+)U",
    (ROOT / "include/solar_os_qemu_audio_protocol.h").read_text())}
BINARY = os.environ.get("SOLAR_OS_QEMU_INPUT_TEST_BINARY")


@unittest.skipUnless(BINARY, "set SOLAR_OS_QEMU_INPUT_TEST_BINARY for emulator tests")
class QemuAudioTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.output = self.directory / "output.wav"
        self.log = (self.directory / "qemu.log").open("w+b")
        self.addCleanup(self.log.close)
        self.process = subprocess.Popen([
            BINARY, "-machine", "esp32s3", "-display", "none", "-S",
            "-serial", "null", "-monitor", "none", "-nic", "none",
            "-audiodev", f"wav,id=output,path={self.output},out.frequency=48000,out.channels=2,out.format=s16",
            "-global", "solar-os-audio.audiodev=output",
            "-qmp", f"unix:{self.directory}/qmp,server=on,wait=off",
            "-qtest", f"unix:{self.directory}/qtest,server=on,wait=off",
        ], stdout=self.log, stderr=subprocess.STDOUT)
        self.addCleanup(self.stop)
        for name in ("qmp", "qtest"):
            connection = socket.socket(socket.AF_UNIX)
            connection.settimeout(5)
            self.addCleanup(connection.close)
            deadline = time.monotonic() + 10
            while True:
                try:
                    connection.connect(str(self.directory / name))
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    if time.monotonic() > deadline or self.process.poll() is not None:
                        self.log.seek(0)
                        self.fail(self.log.read().decode(errors="replace"))
                    time.sleep(.02)
            stream = connection.makefile("rwb", buffering=0)
            self.addCleanup(stream.close)
            setattr(self, name, stream)
        json.loads(self.qmp.readline())
        self.command("qmp_capabilities")
        self.assertEqual(self.reg("MAGIC"), ABI["MAGIC"])
        self.assertEqual(self.reg("VERSION"), ABI["VERSION"])
        self.assertEqual(self.reg("READY"), 1)

    def stop(self):
        if self.process.poll() is None: self.process.terminate()
        self.process.wait(timeout=5)

    def command(self, name):
        self.qmp.write((json.dumps({"execute": name}) + "\n").encode())
        while True:
            response = json.loads(self.qmp.readline())
            self.assertNotIn("error", response)
            if "return" in response: return response["return"]

    def request(self, command):
        self.qtest.write((command + "\n").encode())
        result = self.qtest.readline().decode().split()
        self.assertEqual(result[0], "OK", result)
        return result[1] if len(result) > 1 else None

    def reg(self, name, value=None):
        address = ABI["BASE"] + ABI["REG_" + name]
        if value is None: return int(self.request(f"readl {address:#x}"), 0)
        self.request(f"writel {address:#x} {value:#x}")

    def enqueue(self, frames, left=12000, right=-6000):
        while frames:
            count = min(frames, ABI["STAGE_FRAMES"])
            data = struct.pack("<hh", left, right) * count
            address = ABI["BASE"] + ABI["REG_DATA"]
            self.request(f"write {address:#x} {len(data):#x} 0x{data.hex()}")
            self.reg("COMMIT", count)
            self.assertEqual(self.reg("ERROR"), 0)
            frames -= count

    def wait_for(self, predicate):
        deadline = time.monotonic() + 5
        while not predicate():
            self.assertLess(time.monotonic(), deadline, "audio queue did not drain")
            time.sleep(.01)

    def capture(self):
        time.sleep(.15)  # Let the host backend consume the final mixed samples.
        self.command("quit")
        self.assertEqual(self.process.wait(timeout=5), 0)
        with wave.open(str(self.output)) as recorded:
            self.assertEqual((recorded.getframerate(), recorded.getnchannels(), recorded.getsampwidth()), (48000, 2, 2))
            samples = array.array("h", recorded.readframes(recorded.getnframes()))
            if os.sys.byteorder != "little": samples.byteswap()
            return samples

    def test_backpressure_invalid_commits_and_reset(self):
        self.reg("COMMIT", 1)
        self.assertEqual(self.reg("ERROR"), ABI["ERROR_STATE"])
        self.reg("CONTROL", ABI["START"])
        self.enqueue(ABI["QUEUE_FRAMES"])
        self.assertEqual(self.reg("FREE"), 0)
        self.assertEqual(self.reg("QUEUED"), ABI["QUEUE_FRAMES"])
        self.assertEqual(self.reg("WRITTEN"), ABI["QUEUE_FRAMES"])
        self.assertEqual(self.reg("SUBMITTED"), 0)
        self.reg("COMMIT", 1)
        self.assertEqual(self.reg("ERROR"), ABI["ERROR_FULL"])
        self.reg("CONTROL", ABI["RESET"])
        self.assertEqual(self.reg("ACTIVE"), 0)
        self.assertEqual(self.reg("QUEUED"), 0)
        self.reg("CONTROL", ABI["START"])
        for count in (0, ABI["STAGE_FRAMES"] + 1):
            self.reg("COMMIT", count)
            self.assertEqual(self.reg("ERROR"), ABI["ERROR_ARGUMENT"])
        self.reg("VOLUME", 23)
        self.reg("VOLUME", 101)
        self.assertEqual(self.reg("VOLUME"), 23)
        self.assertEqual(self.reg("ERROR"), ABI["ERROR_ARGUMENT"])

    def test_stereo_samples_volume_and_complete_tail_reach_wav(self):
        self.reg("CONTROL", ABI["START"])
        self.reg("VOLUME", 50)
        self.enqueue(2400)
        self.command("cont")
        self.wait_for(lambda: self.reg("QUEUED") == 0)
        self.assertEqual(self.reg("SUBMITTED"), 2400)
        self.reg("CONTROL", ABI["STOP"])
        self.wait_for(lambda: self.reg("ACTIVE") == 0)
        samples = self.capture()
        self.assertEqual(len(samples), 4800)
        for actual, target in ((samples[0::2], 12000 * 127 / 255), (samples[1::2], -6000 * 127 / 255)):
            self.assertLessEqual(max(abs(value - target) for value in actual), 2)

    def test_mute_reopen_and_stop_drain_preserve_both_streams(self):
        self.reg("CONTROL", ABI["START"])
        self.reg("VOLUME", 0)
        self.enqueue(500)
        self.command("cont")
        self.wait_for(lambda: self.reg("QUEUED") == 0)
        self.reg("CONTROL", ABI["STOP"])
        self.wait_for(lambda: self.reg("ACTIVE") == 0)
        self.reg("CONTROL", ABI["START"])
        self.reg("VOLUME", 100)
        self.enqueue(500, -12000, 6000)
        self.reg("CONTROL", ABI["STOP"])
        self.wait_for(lambda: self.reg("ACTIVE") == 0)
        samples = self.capture()
        self.assertEqual(len(samples), 2000)
        self.assertEqual(max(abs(value) for value in samples[:1000]), 0)
        self.assertTrue(all(abs(value + 12000) <= 2 for value in samples[1000::2]))
        self.assertTrue(all(abs(value - 6000) <= 2 for value in samples[1001::2]))


if __name__ == "__main__": unittest.main()
