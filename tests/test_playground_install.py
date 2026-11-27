import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from zipfile import ZipFile


ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tests/host"


class PlaygroundInstallTest(unittest.TestCase):
    def test_busy_cancellation_and_failed_publication_preserve_package(self):
        subprocess.run(["make", "-C", str(HOST), "playground_install_test"], check=True)
        with tempfile.TemporaryDirectory(prefix="playground-") as root:
            storage, server = Path(root, "sd"), Path(root, "server")
            storage.mkdir()
            server.mkdir()
            fixture = server / "entries.zip"
            payload = b"stored ZIP entry"
            with ZipFile(fixture, "w") as entries:
                entries.writestr("example", payload)
                entries.writestr("empty", b"")
            data = fixture.read_bytes()
            (server / "bad-crc.zip").write_bytes(
                data.replace(payload, b"x" + payload[1:], 1)
            )
            archive = server / "tool.sopkg"
            with ZipFile(archive, "w") as package:
                package.writestr("manifest.json", json.dumps({
                    "id": "tool", "entry": "main.py", "runtime": "python", "version": "1.0.0",
                }))
                package.writestr("main.py", "print('fixture')\n")
            data = archive.read_bytes()
            digest = hashlib.sha256(data).hexdigest()
            (server / "catalog.json").write_text(json.dumps({
                "schema": "solaros.playground.catalog", "schema_version": 1,
                "categories": [{"id": "tools", "title": "Tools", "order": 0}],
                "apps": [{"id": "tool", "entry": "main.py", "runtime": "python", "version": "1.0.0",
                          "name": "Tool", "category": "tools", "author": "SolarOS", "description": "Test tool",
                          "tags": [], "requires": [], "min_solaros": "0.0.0", "archive": archive.name,
                          "sha256": digest, "size": len(data)}],
            }))
            subprocess.run([str(HOST / "playground_install_test"), str(storage), str(server),
                            digest, str(len(data))], check=True)
