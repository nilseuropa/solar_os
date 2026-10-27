from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class TaskAdmissionTest(unittest.TestCase):
    def test_managed_psram_and_internal_reserves(self):
        source = (ROOT / "src/solar_os_task.c").read_text()
        functions = production_functions(source, (
            "task_role_valid", "task_add_size", "task_internal_admitted",
            "task_external_admitted", "solar_os_task_can_create",
            "solar_os_task_get_admission_status"))
        with tempfile.TemporaryDirectory(prefix="solaros-task-admission-") as tmp:
            Path(tmp, "task_admission.inc").write_text(functions)
            for external in (0, 1):
                binary = Path(tmp, f"admission-{external}")
                subprocess.run([
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function",
                    f"-DSOLAR_OS_FREERTOS_EXTERNAL_MEMORY={external}",
                    "-I", tmp,
                    "-I", str(ROOT / "tests/host/jobs_admission_stubs"),
                    "-I", str(ROOT / "tests/host"),
                    "-I", str(ROOT / "src"),
                    "-I", str(ROOT / "src/services"),
                    str(ROOT / "tests/host/task_admission_test.c"),
                    "-o", str(binary),
                ], check=True)
                subprocess.run([str(binary)], check=True)
