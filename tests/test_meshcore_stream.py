import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(encoding="utf-8")
MESHCORE = (ROOT / "src/services/solar_os_meshcore.cpp").read_text(encoding="utf-8")
STREAM = (ROOT / "src/services/solar_os_meshcore_stream.c").read_text(encoding="utf-8")
LINK_STREAM = (ROOT / "src/services/solar_os_link_stream.c").read_text(
    encoding="utf-8"
)
SHELL = (ROOT / "src/shell/solar_os_shell_meshcore.c").read_text(encoding="utf-8")
MANUAL = (ROOT / "doc/manual/meshcore.md").read_text(encoding="utf-8")


class MeshcoreStreamTest(unittest.TestCase):
    def test_meshcore_package_owns_the_link_backed_adapter(self):
        section = PACKAGES.split("[packages.service_meshcore]", 1)[1].split(
            "[packages.", 1
        )[0]
        self.assertIn('"service_link"', section)
        self.assertIn('"services/solar_os_meshcore_stream.c"', section)
        self.assertIn('"services/solar_os_meshcore_stream_codec.c"', section)

    def test_stream_frames_use_encrypted_direct_requests(self):
        self.assertIn("sendRequest(\n            *contact,\n            envelope", MESHCORE)
        self.assertIn("solar_os_meshcore_stream_envelope_matches", MESHCORE)
        self.assertIn("solar_os_meshcore_stream_ingest", MESHCORE)

    def test_both_directions_revalidate_exact_trusted_identity(self):
        self.assertGreaterEqual(
            STREAM.count("SOLAR_OS_CONTACT_TRUST_TRUSTED"), 2
        )
        self.assertIn("memcmp(binding->public_key", STREAM)
        self.assertIn("decoded.source != peer_id", STREAM)
        self.assertIn("decoded.destination != local_peer_id", STREAM)

    def test_shell_and_manual_expose_the_two_sided_workflow(self):
        self.assertIn("meshcore stream create <port> <endpoint-id>", SHELL)
        self.assertIn("meshcore stream remove <port>", SHELL)
        self.assertIn(
            'argc >= 3 && strcmp(argv[2], "status") == 0', SHELL
        )
        self.assertIn("meshcore stream create mser0 <endpoint-id>", MANUAL)
        self.assertIn("session create shell mser0 --term dumb", MANUAL)
        self.assertIn("job start bridge uart0 mser0", MANUAL)

    def test_status_preserves_the_last_stream_decode_issue(self):
        self.assertIn("last_decode_issue = issue", LINK_STREAM)
        self.assertIn("decode-issue=%s", SHELL)


if __name__ == "__main__":
    unittest.main()
