"""Check the real target frame after configuring a T-Deck PlatformIO build."""

import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
DATABASE = ROOT / ".pio/build/t_deck_plus/compile_commands.json"


class MeshcoreStackTest(unittest.TestCase):
    @unittest.skipUnless(DATABASE.exists(), "configure the t_deck_plus build first")
    def test_radio_outbound_leaves_stack_for_crypto_and_storage(self):
        source = ROOT / "src/services/solar_os_meshcore.cpp"
        entries = json.loads(DATABASE.read_text())
        entry = next(item for item in entries if Path(item["file"]) == source)
        command = shlex.split(entry["command"])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "meshcore.o"
            command[command.index("-o") + 1] = str(output)
            command.append("-fstack-usage")
            subprocess.run(command, cwd=entry["directory"], check=True,
                           capture_output=True, text=True)
            frames = {}
            for line in output.with_suffix(".su").read_text().splitlines():
                location, size, _kind = line.split("\t")
                frames[location] = int(size)
            # With the large buffer removed the compiler can inline the send
            # path into loop_once. Budget both emitted forms.
            outbound = {location: size for location, size in frames.items()
                        if "SolarMesh::processOutbound()" in location or
                        "solar_os_meshcore_loop_once()" in location}
            self.assertTrue(outbound)
            # The worker has 7168 bytes total, including all nested crypto,
            # filesystem and driver frames. This frame used to consume 4624.
            for location, size in outbound.items():
                self.assertLessEqual(size, 1024, location)


if __name__ == "__main__":
    unittest.main()
