from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions


ROOT = Path(__file__).resolve().parents[1]
COMMANDS = (ROOT / "src/shell/solar_os_shell_commands.c").read_text(
    encoding="utf-8"
)


OTA = (ROOT / "src/services/solar_os_ota.c").read_text(encoding="utf-8")


class OtaWorkerAndProgressTest(unittest.TestCase):
    def test_check_uses_a_dedicated_network_worker(self):
        self.assertIn("#define OTA_WORKER_STACK 16384", OTA)
        self.assertIn("solar_os_task_create_pinned_internal(ota_worker", OTA)
        self.assertIn("worker->check ? ota_check(worker->check)", OTA)
        self.assertIn("solar_os_ota_check(result)", COMMANDS)

    def test_worker_joins_before_return_and_rejects_overlapping_updates(self):
        start = OTA.index("typedef struct {\n    solar_os_ota_check_result_t *check;")
        end = OTA.index("} ota_worker_t;", start) + len("} ota_worker_t;")
        worker = OTA[start:end] + "\n" + production_functions(OTA, (
            "ota_worker", "ota_run", "solar_os_ota_check", "solar_os_ota_upgrade"))
        host = ROOT / "tests/host"
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, "ota_worker.inc").write_text(worker)
            binary = Path(directory, "ota_worker")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-pthread", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                "-I", directory, "-I", str(host), "-I", str(ROOT / "include"),
                "-I", str(ROOT / "src/services"),
                str(host / "ota_worker_test.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_upgrade_progress_reserves_one_column_before_wrapping(self):
        start = COMMANDS.index("static void ota_render_progress_line(")
        end = COMMANDS.index("static void ota_shell_progress_cb(", start)
        renderer = COMMANDS[start:end]

        self.assertIn("solar_os_shell_io_cols(term)", renderer)
        self.assertIn("cols > 1U ? cols - 1U : cols", renderer)
        self.assertIn(
            "line_budget > fixed_width ? line_budget - fixed_width : 0U",
            renderer,
        )
        self.assertIn("solar_os_shell_io_write_len(term, line, line_len)", renderer)

    def test_upgrade_progress_is_emitted_as_one_bounded_string(self):
        start = COMMANDS.index("static void ota_shell_progress_cb(")
        end = COMMANDS.index("static bool ota_parse_slot(", start)
        callback = COMMANDS[start:end]

        self.assertIn("ota_render_progress_line(", callback)
        self.assertNotIn('solar_os_shell_io_printf(state->term, " v%s"', callback)


if __name__ == "__main__":
    unittest.main()
