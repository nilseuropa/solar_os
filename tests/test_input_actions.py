import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
INPUT = (ROOT / "src/services/solar_os_input.c").read_text(encoding="utf-8")
ACTIONS = (ROOT / "src/services/solar_os_input_actions.c").read_text(
    encoding="utf-8"
)
SHELL_INPUT = (ROOT / "src/shell/solar_os_shell_input.c").read_text(
    encoding="utf-8"
)
MAIN = (ROOT / "src/main.c").read_text(encoding="utf-8")
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(encoding="utf-8")


class InputActionsTest(unittest.TestCase):
    def test_gesture_observers_are_not_the_foreground_queue(self):
        writer = INPUT.split("esp_err_t solar_os_input_write_gesture", 1)[1].split(
            "esp_err_t solar_os_input_pointer_calibration_get", 1
        )[0]
        self.assertIn("input_gesture_queue_push_locked", writer)
        self.assertIn("observers[i].observer(&queued", writer)
        self.assertLess(
            writer.index("portEXIT_CRITICAL(&input_lock)"),
            writer.index("observers[i].observer(&queued"),
        )

    def test_actions_match_then_enqueue_without_running_in_observer(self):
        observer = ACTIONS.split("static void input_action_observe", 1)[1].split(
            "esp_err_t solar_os_input_actions_init", 1
        )[0]
        worker = ACTIONS.split("static void input_action_worker", 1)[1].split(
            "static esp_err_t input_action_ensure_worker", 1
        )[0]
        self.assertIn("xQueueSend(queue", observer)
        self.assertNotIn("runner(command)", observer)
        self.assertIn("xQueueReceive", worker)
        self.assertIn("runner(command)", worker)

    def test_shell_exposes_emit_and_transient_binding_crud(self):
        for subcommand in ('"emit"', '"bind"', '"bindings"', '"unbind"'):
            self.assertIn(subcommand, SHELL_INPUT)
        self.assertIn("solar_os_input_actions_emit_key", SHELL_INPUT)
        self.assertIn("solar_os_input_actions_bind", SHELL_INPUT)
        self.assertIn("solar_os_input_actions_clear", SHELL_INPUT)

    def test_runtime_wires_background_runner_and_package_source(self):
        self.assertIn("solar_os_input_actions_init()", MAIN)
        self.assertIn(
            "solar_os_input_actions_set_runner(solar_os_shell_run_background_command)",
            MAIN,
        )
        self.assertIn('"services/solar_os_input_actions.c"', PACKAGES)


if __name__ == "__main__":
    unittest.main()
