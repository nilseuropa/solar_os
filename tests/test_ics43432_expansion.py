from copy import deepcopy
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from solaros_board_manifest import (
    ManifestError, generate_header, load_board_manifest, load_driver_catalog,
    required_packages, validate_board,
)


class Ics43432ExpansionTest(unittest.TestCase):
    def test_reusable_capture_playback_and_power_bindings(self):
        manifests = ROOT / "boards/manifests"
        drivers = load_driver_catalog(ROOT / "boards/expansion_drivers.toml")
        board = load_board_manifest(manifests / "esp32_s3_devkitc1_n16r8.toml", manifests)
        for pin in board["pins"]:
            if pin["gpio"] in (14, 15, 16, 17, 21):
                pin.update(policy="fixed", user=False, adc=False, pwm=False)
        device = {"driver": "ics43432", "name": "audio0", "bindings": {
            "i2s": 1, "bck": 14, "ws": 15, "din": 17, "dout": 16,
            "slot": 1, "power": 21,
        }}
        board.setdefault("devices", []).append(device)
        validate_board(board, drivers)
        self.assertIn("expansion_inmp441", required_packages(board, drivers))
        header = generate_header(board, drivers)
        self.assertIn('.driver = "ics43432", .name = "audio0"', header)
        self.assertIn('.role = "power", .value = 21', header)
        for key, value in (("slot", 2), ("power", 14)):
            invalid = deepcopy(board)
            invalid["devices"][-1]["bindings"][key] = value
            with self.assertRaises(ManifestError):
                validate_board(invalid, drivers)
        # A microphone-only module needs neither a DAC nor a power switch.
        del device["bindings"]["power"]
        del device["bindings"]["dout"]
        validate_board(board, drivers)


if __name__ == "__main__":
    unittest.main()
