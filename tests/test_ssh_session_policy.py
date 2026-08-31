from pathlib import Path
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
SSH_SOURCE = (REPOSITORY / "src/apps/solar_os_ssh.c").read_text(encoding="utf-8")


class SshSessionPolicyTest(unittest.TestCase):
    def test_resumable_ssh_owns_a_private_terminal(self):
        app_definition = SSH_SOURCE.split(
            "const solar_os_app_t solar_os_ssh_app =", 1
        )[1]
        self.assertIn(".flags = SOLAR_OS_APP_FLAG_RESUMABLE,", app_definition)
        self.assertNotIn("SOLAR_OS_APP_FLAG_SHELL_INLINE", app_definition)

    def test_ssh_uses_the_shared_tui_exit_contract(self):
        app_definition = SSH_SOURCE.split(
            "const solar_os_app_t solar_os_ssh_app =", 1
        )[1]
        self.assertIn(".app_class = SOLAR_OS_APP_CLASS_TUI,", app_definition)
        close_policy = SSH_SOURCE.split(
            "static void ssh_request_close", 1
        )[1].split("static bool ssh_is_printable", 1)[0]
        self.assertIn("solar_os_context_finish(ctx, exit_code, status)", close_policy)
        self.assertNotIn("request_terminal_preserve", SSH_SOURCE)

    def test_connection_errors_stop_and_return_immediately(self):
        error_case = SSH_SOURCE.split(
            "case SOLAR_OS_SSH_EVENT_ERROR:", 1
        )[1].split("case SOLAR_OS_SSH_EVENT_DISCONNECTED:", 1)[0]
        self.assertIn("solar_os_ssh_stop(ssh_app.session)", error_case)
        self.assertIn("ssh_request_close(ctx, 1, status)", error_case)


if __name__ == "__main__":
    unittest.main()
