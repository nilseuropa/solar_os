from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ScriptRunnerStateTest(unittest.TestCase):
    def assert_runner_has_isolated_state(
        self, filename, prefix, state_macro, app_state_type, run_symbol
    ):
        source = (ROOT / f"src/apps/solar_os_{filename}.c").read_text(
            encoding="utf-8"
        )
        runner = source.split(f"esp_err_t {run_symbol}", 1)[1].split(
            "static ", 1
        )[0]

        self.assertIn(f"static void *{prefix}_app_state;", source)
        self.assertIn(
            f"static {app_state_type} *{prefix}_runtime_state;", source
        )
        self.assertIn(f".state_slot = &{prefix}_app_state", source)
        self.assertIn("SOLAR_OS_MEMORY_EXTERNAL_PREFERRED", runner)
        self.assertIn(f'"{filename}.runner-state"', runner)
        self.assertLess(
            runner.index(f"{prefix}_runtime_state = runner_state"),
            runner.index(f"memset(&{state_macro}"),
        )
        self.assertLess(
            runner.index(f"{prefix}_runtime_release("),
            runner.index("solar_os_memory_free(runner_state)"),
        )

    def test_python_runner_has_isolated_external_state(self):
        self.assert_runner_has_isolated_state(
            "python",
            "python",
            "python_app",
            "python_cold_state_t",
            "solar_os_python_run",
        )

    def test_lua_runner_has_isolated_external_state(self):
        self.assert_runner_has_isolated_state(
            "lua", "solua", "solua", "solua_cold_state_t", "solar_os_lua_run"
        )


if __name__ == "__main__":
    unittest.main()
