from pathlib import Path
import subprocess
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
SHELL = (REPOSITORY / "src/apps/solar_os_shell.c").read_text(encoding="utf-8")
SSH_HEADER = (REPOSITORY / "src/services/solar_os_ssh.h").read_text(
    encoding="utf-8"
)
SSH_TRANSPORT = (REPOSITORY / "src/services/solar_os_ssh_transport.c").read_text(
    encoding="utf-8"
)
NET = (REPOSITORY / "src/services/solar_os_net.c").read_text(encoding="utf-8")


class SshCompletionPolicyTest(unittest.TestCase):
    def test_network_hosts_reader_and_resolver(self):
        source = NET.split("esp_err_t solar_os_net_hosts_visit", 1)[1].split(
            "static esp_err_t ping_resolve_host", 1
        )[0]
        source = "esp_err_t solar_os_net_hosts_visit" + source
        harness = (REPOSITORY / "tests/host/network_hosts_test.inc").read_text()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "hosts").write_text(
                "# ignored\n192.0.2.1 remote server # comment\n"
                "\n192.0.2.2 relay\n192.0.2.3 remote\n"
            )
            (path / "test.c").write_text(harness.replace("/* HOSTS_SOURCE */", source))
            subprocess.run([
                "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
                "-Werror", str(path / "test.c"), "-o", str(path / "test"),
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(path / "test"), str(path / "hosts")], check=True)

    def test_host_completion_behavior_with_and_without_ssh(self):
        source = SHELL.split(
            "#if SOLAR_OS_PACKAGE_SERVICE_SSH || SOLAR_OS_PACKAGE_SERVICE_NET", 1
        )[1].split("#if SOLAR_OS_PACKAGE_SERVICE_OTA", 1)[0]
        source = source.rsplit("#endif", 1)[0]
        harness = (REPOSITORY / "tests/host/network_completion_test.inc").read_text()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(harness.replace("/* COMPLETION_SOURCE */", source))
            for ssh in (0, 1):
                with self.subTest(ssh=ssh):
                    subprocess.run([
                        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-format-truncation",
                        f"-DSOLAR_OS_PACKAGE_SERVICE_SSH={ssh}",
                        str(path / "test.c"), "-o", str(path / "test"),
                    ], check=True, capture_output=True, text=True)
                    subprocess.run([str(path / "test")], check=True)

    def test_resolver_and_completion_share_the_hosts_visitor(self):
        self.assertIn("solar_os_ssh_host_visitor_t", SSH_HEADER)
        self.assertIn("solar_os_ssh_hosts_visit", SSH_HEADER)

        lookup = SSH_TRANSPORT.split(
            "static esp_err_t transport_lookup_hosts_file", 1
        )[1].split("esp_err_t solar_os_ssh_hosts_visit", 1)[0]
        visitor = SSH_TRANSPORT.split(
            "esp_err_t solar_os_ssh_hosts_visit", 1
        )[1].split("int solar_os_ssh_transport_wait_socket", 1)[0]
        self.assertIn("transport_visit_hosts_path", lookup)
        self.assertIn("transport_visit_hosts_path", visitor)

    def test_ssh_and_scp_complete_hosts_without_a_cache(self):
        completion = SHELL.split(
            "static bool SHELL_NOINLINE shell_complete_ssh_host_argument", 1
        )[1].split("#if SOLAR_OS_PACKAGE_SERVICE_OTA", 1)[0]
        self.assertIn("solar_os_ssh_hosts_visit", completion)
        self.assertIn("authority_prefix_len", completion)
        self.assertIn('scp_command ? ":" : " "', completion)
        self.assertNotIn("static char", completion)


if __name__ == "__main__":
    unittest.main()
