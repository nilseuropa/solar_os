from pathlib import Path
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
SHELL = (REPOSITORY / "src/apps/solar_os_shell.c").read_text(encoding="utf-8")
SSH_HEADER = (REPOSITORY / "src/services/solar_os_ssh.h").read_text(
    encoding="utf-8"
)
SSH_TRANSPORT = (REPOSITORY / "src/services/solar_os_ssh_transport.c").read_text(
    encoding="utf-8"
)


class SshCompletionPolicyTest(unittest.TestCase):
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
