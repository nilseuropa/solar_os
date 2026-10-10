"""Run the native zoo and stored-ZIP services against adversarial releases."""
import copy
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tests/host/zoo_test"


class ZooInstallTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-C", str(ROOT / "tests/host"), "zoo_test"], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="zoo-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.server = self.root / "server"
        self.server.mkdir()
        self.storage = self.root / "sd"
        self.storage.mkdir()
        self.data = b"Numerical model fixture, never executed." * 1000
        self.bundle = json.loads((ROOT / "tests/fixtures/model_bundle/raw.json").read_bytes())
        self.bundle["model"]["sha256"] = hashlib.sha256(self.data).hexdigest()
        self.bundle["assets"] = {"LICENSE": hashlib.sha256(b"fixture license").hexdigest()}
        self.bundle["license"] = {"id": "LicenseRef-Test", "file": "LICENSE"}
        self.metadata = {"schema": 1, "id": self.bundle["id"], "version": self.bundle["version"],
                         "name": "Arithmetic", "summary": "Generic named-tensor model",
                         "tasks": ["arithmetic"], "modalities": ["tensor"], "requirements": {},
                         "validation": [], "provenance": {"author": "Test", "origin": "original"}}

    def publish(self, mutate_files=None, mutate_catalog=None, extra=None):
        bundle = (json.dumps(self.bundle) + "\n").encode()
        files = {"bundle.json": bundle, "zoo.json": json.dumps(self.metadata).encode(),
                 self.bundle["model"]["file"]: self.data, "LICENSE": b"fixture license"}
        if mutate_files:
            mutate_files(files)
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_STORED) as archive:
            for name, data in files.items():
                info = zipfile.ZipInfo(name)
                info.create_system = 3
                info.external_attr = 0o100644 << 16
                if name == extra:
                    info.external_attr = 0o120777 << 16
                archive.writestr(info, data)
        data = buffer.getvalue()
        digest = hashlib.sha256(data).hexdigest()
        entry = {key: self.bundle[key] for key in ("runtime", "inputs", "outputs", "result", "license")}
        entry.update(metadata=self.metadata, packages=["service_inference"], psram=True, status="declared",
                     bundle_sha256=hashlib.sha256(bundle).hexdigest(),
                     artifact={"file": f"models/{self.bundle['id']}/{self.bundle['version']}/{digest}.zip",
                               "sha256": digest, "bytes": len(data), "unpacked_bytes": sum(map(len, files.values()))})
        catalog = {"schema": 1, "models": [entry]}
        if mutate_catalog:
            mutate_catalog(catalog)
        (self.server / (digest + ".zip")).write_bytes(data)
        (self.server / "catalog.json").write_text(json.dumps(catalog))
        return digest

    def run_native(self, mode="normal"):
        process = subprocess.run([str(HOST), str(self.storage), str(self.server), mode],
                                 text=True, capture_output=True)
        self.assertEqual(process.returncode, 0, process.stderr + process.stdout)
        return json.loads(process.stdout)

    @property
    def installation(self):
        return self.storage / "dl/zoo/models" / self.bundle["id"] / self.bundle["version"]

    def test_install_cache_reload_and_no_inference_or_script_dependency(self):
        self.publish()
        first = self.run_native()
        self.assertEqual(first["error"], 0)
        self.assertTrue(first["installed"])
        self.assertEqual((self.installation / self.bundle["model"]["file"]).read_bytes(), self.data)
        second = self.run_native()
        self.assertEqual(second["cached"], 0)
        self.assertEqual(second["error"], 0)
        self.assertFalse(list(self.installation.parent.glob("*.stage")))
        self.assertFalse(list(self.installation.parent.glob("*.old")))
        self.assertFalse(list(self.installation.parent.glob("*.zip")))

    def test_header_callbacks_with_unknown_status_accept_completed_http_200(self):
        self.publish()
        result = self.run_native()
        self.assertEqual(result["error"], 0)
        self.assertTrue(result["installed"])

    def test_irregular_borrowed_http_chunks_preserve_download_bytes(self):
        self.publish()
        result = self.run_native("fragmented")
        self.assertEqual(result["error"], 0)
        self.assertTrue(result["installed"])
        self.assertEqual((self.installation / self.bundle["model"]["file"]).read_bytes(), self.data)

    def test_corrupt_archive_preserves_existing_install(self):
        self.publish()
        self.assertEqual(self.run_native()["error"], 0)
        old = (self.installation / "bundle.json").read_bytes()
        digest = self.publish()
        (self.server / (digest + ".zip")).write_bytes(b"corrupt")
        self.assertNotEqual(self.run_native()["error"], 0)
        self.assertEqual((self.installation / "bundle.json").read_bytes(), old)

    def test_silent_manifest_write_corruption_preserves_existing_install(self):
        self.publish()
        self.assertEqual(self.run_native()["error"], 0)
        old = (self.installation / "bundle.json").read_bytes()
        self.assertNotEqual(self.run_native("extract-manifest-corruption")["error"], 0)
        self.assertEqual((self.installation / "bundle.json").read_bytes(), old)
        self.assertFalse(list(self.installation.parent.glob("*.stage")))

    def test_corrupt_model_asset_manifest_and_extra_members_are_rejected(self):
        for name in (self.bundle["model"]["file"], "LICENSE", "bundle.json", "unexpected.txt", "../outside"):
            with self.subTest(name=name):
                self.publish(mutate_files=lambda files: files.update({name: b"corrupt"}))
                self.assertNotEqual(self.run_native()["error"], 0)
                self.assertFalse(self.installation.exists())

    def test_symlink_metadata_in_archive_is_rejected(self):
        self.publish(extra="LICENSE")
        self.assertNotEqual(self.run_native()["error"], 0)
        self.assertFalse(self.installation.exists())

    def test_incompatible_releases_remain_browsable_but_are_not_installed(self):
        for field, value in (("target", "esp32p4"), ("version", "9.0.0"), ("backend", "other")):
            with self.subTest(field=field):
                self.publish(mutate_catalog=lambda c: c["models"][0]["runtime"].update({field: value}))
                result = self.run_native()
                self.assertEqual(result["count"], 1)
                self.assertNotEqual(result["error"], 0)
                self.assertFalse(self.installation.exists())
        self.publish(mutate_catalog=lambda c: c["models"][0].update(packages=["service_missing"]))
        self.assertNotEqual(self.run_native()["error"], 0)
        self.assertFalse(self.installation.exists())

    def test_cancellation_low_storage_and_failed_directory_swap_preserve_old_install(self):
        self.publish()
        self.assertEqual(self.run_native()["error"], 0)
        old = (self.installation / "bundle.json").read_bytes()
        for mode in ("cancel-download", "no-space", "swap-failure", "http-error"):
            with self.subTest(mode=mode):
                self.assertNotEqual(self.run_native(mode)["error"], 0)
                self.assertEqual((self.installation / "bundle.json").read_bytes(), old)
                self.assertFalse(list(self.installation.parent.glob("*.stage")))

    def test_source_change_invalidates_cache(self):
        self.publish()
        self.assertNotEqual(self.run_native("source-change")["error"], 0)

    def test_catalog_backup_is_recovered_after_interrupted_swap(self):
        self.publish()
        self.assertEqual(self.run_native("refresh-only")["error"], 0)
        cache = self.storage / "dl/zoo/catalog.json"
        cache.rename(cache.parent / ".catalog.old")
        (self.server / "catalog.json").write_text("invalid")
        result = self.run_native("refresh-only")
        self.assertEqual(result["cached"], 0)
        self.assertNotEqual(result["error"], 0)
        self.assertTrue(cache.exists())

    def test_invalid_refresh_keeps_cached_catalog_and_duplicate_identity_is_rejected(self):
        self.publish()
        self.assertEqual(self.run_native("refresh-only")["error"], 0)
        cached = (self.storage / "dl/zoo/catalog.json").read_bytes()
        self.publish(mutate_catalog=lambda c: c["models"].append(copy.deepcopy(c["models"][0])))
        self.assertNotEqual(self.run_native("refresh-only")["error"], 0)
        self.assertEqual((self.storage / "dl/zoo/catalog.json").read_bytes(), cached)
        (self.server / "catalog.json").write_text('{"schema":1,"schema":1,"models":[]}')
        self.assertNotEqual(self.run_native("refresh-only")["error"], 0)
        self.assertEqual((self.storage / "dl/zoo/catalog.json").read_bytes(), cached)

    def test_catalog_larger_than_storage_write_limit_is_cached(self):
        def large(catalog):
            for i in range(100):
                e = copy.deepcopy(catalog["models"][0])
                e["metadata"]["id"] = "model-" + str(i)
                e["artifact"]["file"] = "models/" + e["metadata"]["id"] + "/1.0.0/" + e["artifact"]["sha256"] + ".zip"
                catalog["models"].append(e)
        self.publish(mutate_catalog=large)
        self.assertGreater((self.server / "catalog.json").stat().st_size, 65536)
        self.assertEqual(self.run_native("refresh-only")["error"], 0)
        self.assertEqual(self.run_native("refresh-only")["cached"], 0)


if __name__ == "__main__":
    unittest.main()
