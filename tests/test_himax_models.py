"""Run the production Himax programmer and native data helpers with host adapters."""
import copy
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tests/host/himax_model_test"


class HimaxModelsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-C", str(ROOT / "tests/host"), "himax_model_test"], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="hm-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.folder = self.root / "bundle"
        self.folder.mkdir()
        self.path = self.folder / "bundle.json"
        self.binary = self.folder / "model.tflite"
        shutil.copyfile(ROOT / "tests/fixtures/himax/ports.tflite", self.binary)
        assets = {"LICENSE": b"Non-executable host fixture.\n", "labels.txt": b"\n".join(str(n).encode() for n in range(10)) + b"\n"}
        for name, data in assets.items():
            (self.folder / name).write_bytes(data)
        self.manifest = {"schema": 2, "id": "fixture", "version": "1.0.0",
            "runtime": {"backend": "himax", "version": "sscma-v0", "target": "hx6538",
                "provider": {"id": "processor-himax", "abi": 1, "capabilities": ["capture", "model-select", "model-upload"]}},
            "model": {"file": "model.tflite", "format": "tflite-vela", "sha256": hashlib.sha256(self.binary.read_bytes()).hexdigest()},
            "inputs": {"camera": {"adapter": {"type": "camera"}, "width": 32, "height": 32, "color": "RGB"}},
            "outputs": {"classes": {"format": "sscma-classes"}},
            "result": {"type": "sscma-classification", "options": {"labels": "labels.txt", "class_count": 10}},
            "assets": {n: hashlib.sha256(d).hexdigest() for n, d in assets.items()},
            "license": {"id": "LicenseRef-Test", "file": "LICENSE"}}
        self.write()

    def write(self):
        self.path.write_text(json.dumps(self.manifest))

    def run_native(self, mode="normal"):
        p = subprocess.run([str(HOST), str(self.root), str(self.path), mode], text=True, capture_output=True, timeout=30)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return json.loads(p.stdout)

    @property
    def receipt(self):
        return self.root / ".himax/0123456789abcdef.json"

    @property
    def record(self):
        return next(r for r in json.loads(self.receipt.read_bytes())["slots"] if r["slot"] == 4)

    def test_upload_crc_inference_restore_and_atomic_receipt(self):
        result = self.run_native()
        self.assertEqual(result["error"], 0)
        self.assertEqual(result["selected"], 1)
        record = self.record
        self.assertEqual(record["state"], "programmed")
        self.assertEqual(record["device"], "0123456789abcdef")
        self.assertEqual(record["sha256"], self.manifest["model"]["sha256"])
        sent = (self.root / "programmed.bin").read_bytes()
        actual = self.binary.read_bytes()
        self.assertEqual(sent[:len(actual)], actual)
        self.assertEqual(sent[len(actual):], b"\xff" * (-len(actual) % 256))
        self.assertFalse(list(self.folder.glob("*.native.*")))

    def test_invalid_active_and_unapproved_slots_do_not_write(self):
        for mode in ("invalid-slot", "zero-slot", "active-slot", "no-replace", "busy", "old-firmware", "bad-layout", "missing-spi"):
            with self.subTest(mode=mode):
                result = self.run_native(mode)
                self.assertNotEqual(result["error"], 0)
                self.assertEqual(result["pages"], 0)
                self.assertEqual(result["selected"], 1)
                self.assertFalse(self.receipt.exists())

    def test_slot_one_upload_uses_first_model_region_and_restores_previous_selection(self):
        for mode in ("slot-one", "slot-one-reset-default"):
            with self.subTest(mode=mode):
                result = self.run_native(mode)
                self.assertEqual(result["error"], 0)
                self.assertEqual(result["selected"], 4)
                record = next(r for r in json.loads(self.receipt.read_bytes())["slots"] if r["slot"] == 1)
                self.assertEqual(record["state"], "programmed")
                self.assertEqual(record["previous_slot"], 4)
                actual = self.binary.read_bytes()
                self.assertEqual((self.root / "programmed.bin").read_bytes(), actual + b"\xff" * (-len(actual) % 256))

    def test_reset_default_is_reselected_before_programming(self):
        result = self.run_native("reset-default")
        self.assertEqual(result["error"], 0)
        self.assertEqual(result["selected"], 1)
        self.assertEqual(self.record["state"], "programmed")

    def test_protocol_failures_and_cancellation_restore_and_never_record_success(self):
        for mode in ("crc-error", "crc-state3", "timeout", "spi-error", "cancel", "reset-failure", "inference-failure", "wrong-result", "malformed-result", "select-failure"):
            with self.subTest(mode=mode):
                result = self.run_native(mode)
                self.assertNotEqual(result["error"], 0)
                self.assertEqual(result["selected"], 1)
                self.assertEqual(self.record["state"], "failed")

    def test_corrupt_model_asset_labels_and_dimensions_reject_before_hardware(self):
        mutations = (lambda b: b["inputs"]["camera"].update(width=96),
                     lambda b: b["result"]["options"].update(class_count=2),
                     lambda b: b["model"].update(sha256="f" * 64),
                     lambda b: b["assets"].update({"labels.txt": "f" * 64}),
                     lambda b: b["model"].update(file="../outside.tflite"),
                     lambda b: b["runtime"].update(backend="espdl"))
        original = copy.deepcopy(self.manifest)
        for mutate in mutations:
            self.manifest = copy.deepcopy(original)
            mutate(self.manifest)
            self.write()
            result = self.run_native()
            self.assertNotEqual(result["error"], 0)
            self.assertEqual(result["probes"], 0)
            self.assertEqual(result["pages"], 0)

    def test_strict_json_duplicates_trailing_data_and_nesting(self):
        valid = self.path.read_text()
        for broken in (valid + " {}", valid.replace('"schema": 2', '"schema": 2, "schema": 2', 1),
                       '{"a":' * 40 + '0' + '}' * 40):
            self.path.write_text(broken)
            result = self.run_native()
            self.assertNotEqual(result["error"], 0)
            self.assertEqual(result["probes"], 0)

    def test_missing_used_ethos_operator_rejects_before_hardware(self):
        data = self.binary.read_bytes().replace(b"ethos-u", b"cpu-op?")
        self.binary.write_bytes(data)
        self.manifest["model"]["sha256"] = hashlib.sha256(data).hexdigest()
        self.write()
        result = self.run_native()
        self.assertNotEqual(result["error"], 0)
        self.assertEqual(result["probes"], 0)

    def test_explicit_selection_does_not_program(self):
        result = self.run_native("select")
        self.assertEqual(result["selected"], 4)
        self.assertEqual(result["pages"], 0)

    def test_invalid_firmware_bounds_and_shapes_reject_before_writes(self):
        original = copy.deepcopy(self.manifest)
        mutations = (lambda b: b["runtime"].update(firmware={"minimum": 5}),
                     lambda b: b["runtime"].update(firmware={"minimum": "2024.02.30"}),
                     lambda b: b["runtime"].update(firmware={"minimum": "2025.01.01", "maximum": "2024.01.01"}),
                     lambda b: b["runtime"]["provider"].update(capabilities={"a": "capture", "b": "model-select", "c": "model-upload"}),
                     lambda b: b.update(outputs={"boxes": {"format": "sscma-boxes"}}),
                     lambda b: b.update(assets=[]))
        for mutate in mutations:
            self.manifest = copy.deepcopy(original)
            mutate(self.manifest)
            self.write()
            result = self.run_native()
            self.assertNotEqual(result["error"], 0)
            self.assertEqual(result["pages"], 0)
            self.assertEqual(result["probes"], 0)

    def test_bad_labels_utf8_and_flatbuffer_offsets_reject_before_hardware(self):
        path = self.folder / "labels.txt"
        for labels in (b"\n" * 10, b"\xff\n" * 10, b"x\x00\n" * 10, b"x" * 256 + b"\n"):
            path.write_bytes(labels)
            self.manifest["assets"]["labels.txt"] = hashlib.sha256(labels).hexdigest()
            self.write()
            self.assertEqual(self.run_native()["probes"], 0)
        path.write_bytes(b"\n".join(str(n).encode() for n in range(10)) + b"\n")
        self.manifest["assets"]["labels.txt"] = hashlib.sha256(path.read_bytes()).hexdigest()
        data = self.binary.read_bytes()
        for header in (b"\x00" * 4, b"\xff" * 4):
            changed = header + data[4:]
            self.binary.write_bytes(changed)
            self.manifest["model"]["sha256"] = hashlib.sha256(changed).hexdigest()
            self.write()
            result = self.run_native()
            self.assertNotEqual(result["error"], 0)
            self.assertEqual(result["probes"], 0)

    def test_storage_failures_do_not_leave_false_success(self):
        result = self.run_native("record-failure")
        self.assertNotEqual(result["error"], 0)
        self.assertEqual(result["pages"], 0)
        result = self.run_native("final-record-failure")
        self.assertNotEqual(result["error"], 0)
        self.assertEqual(result["selected"], 1)
        self.assertEqual(self.record["state"], "uploading")

    def test_unrecoverable_selection_is_reported_without_a_success_receipt(self):
        result = self.run_native("restore-failure")
        self.assertNotEqual(result["error"], 0)
        self.assertEqual(self.record["state"], "failed")

    def test_allocation_failures_release_files_json_and_camera(self):
        for n in range(180):
            if self.receipt.exists():
                self.receipt.unlink()
            with self.subTest(allocation=n):
                result = self.run_native("oom-" + str(n))
                self.assertEqual(result["selected"], 1)
                if result["error"] and self.receipt.exists():
                    self.assertNotEqual(self.record["state"], "programmed")

    def test_live_slot_listing_and_inventory_survive_bundle_removal(self):
        empty = self.run_native("slots")
        self.assertEqual(empty["inventory_state"], "missing")
        self.assertTrue(all(r["record"] is None for r in empty["slots"]))
        self.assertEqual(self.run_native()["error"], 0)
        shutil.rmtree(self.folder)
        listed = self.run_native("slots-alias")
        self.assertEqual(listed["source"], "camera1")
        self.assertEqual(listed["slots"][3]["record"]["id"], "fixture")
        self.assertEqual(listed["active_slot"], 1)
        changed = self.run_native("slots-active")
        self.assertEqual(changed["active_slot"], 2)
        self.assertTrue(changed["slots"][1]["active"])
        self.assertTrue(all(r["record"] is None for r in self.run_native("slots-other-device")["slots"]))

    def test_updates_preserve_other_slots_and_recover_backup(self):
        self.assertEqual(self.run_native()["error"], 0)
        record = copy.deepcopy(self.record)
        record.update(slot=3, id="another-model")
        inventory = json.loads(self.receipt.read_bytes())
        inventory["slots"].append(record)
        self.receipt.write_text(json.dumps(inventory))
        backup = self.receipt.with_name(self.receipt.name + ".native.old")
        self.receipt.rename(backup)
        listed = self.run_native("slots")
        self.assertEqual(listed["slots"][2]["record"]["id"], "another-model")
        self.assertEqual(self.run_native()["error"], 0)
        self.assertEqual(self.run_native("slots")["slots"][2]["record"]["id"], "another-model")
        self.assertFalse(backup.exists())

    def test_invalid_inventory_cannot_authorize_programming_or_claim_known_models(self):
        self.assertEqual(self.run_native()["error"], 0)
        original = json.loads(self.receipt.read_bytes())
        for mutate in (lambda v: v.update(device="another-device"),
                       lambda v: v["slots"].append(copy.deepcopy(v["slots"][0])),
                       lambda v: v["slots"][0].update(sha256="bad"),
                       lambda v: v["slots"][0].update(written_bytes=1)):
            broken = copy.deepcopy(original)
            mutate(broken)
            self.receipt.write_text(json.dumps(broken))
            listed = self.run_native("slots")
            self.assertEqual(listed["inventory_state"], "invalid")
            self.assertTrue(all(r["record"] is None for r in listed["slots"]))
            failed = self.run_native()
            self.assertNotEqual(failed["error"], 0)
            self.assertEqual(failed["pages"], 0)
            self.assertEqual(json.loads(self.receipt.read_bytes()), broken)

    def test_unavailable_flash_is_explicit_and_prevents_chip_writes(self):
        self.assertEqual(self.run_native("slots-no-flash")["inventory_state"], "unavailable")
        self.assertEqual(self.run_native("slots-old-host")["inventory_state"], "unavailable")
        result = self.run_native("no-flash")
        self.assertNotEqual(result["error"], 0)
        self.assertEqual(result["pages"], 0)

    def test_inventory_query_allocation_failures_release_camera_and_json(self):
        self.assertEqual(self.run_native()["error"], 0)
        for n in range(100):
            with self.subTest(allocation=n):
                result = self.run_native("slots-oom-" + str(n))
                if "error" not in result:
                    self.assertEqual(result["slots"][3]["record"]["state"], "programmed")


if __name__ == "__main__":
    unittest.main()
