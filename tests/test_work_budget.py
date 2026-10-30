from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class WorkBudgetTest(unittest.TestCase):
    def test_file_transfer_cooperates_and_preserves_payload_and_error_cleanup(self):
        source = (ROOT / "src/jobs/solar_os_httpd_job.c").read_text()
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "httpd_send_file.inc").write_text(production_functions(source, ("send_file",)))
            binary = Path(tmp, "http-work")
            subprocess.run(["cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                            "-I", tmp, "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src"),
                            "-I", str(ROOT / "src/services"),
                            str(ROOT / "tests/host/httpd_work_test.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_elapsed_time_budget_and_full_tick_at_different_tick_rates(self):
        source = (ROOT / "src/solar_os_task.c").read_text()
        functions = production_functions(source, ("solar_os_work_begin", "solar_os_work_checkpoint"))
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "work_budget.inc").write_text(functions)
            for rate in (100, 1000, 2000):
                binary = Path(tmp, f"work-{rate}")
                subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                f"-DTEST_TICK_HZ={rate}", "-I", tmp, "-I", str(ROOT / "src"),
                                str(ROOT / "tests/host/work_budget_test.c"), "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True)
