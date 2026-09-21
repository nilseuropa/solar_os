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
SHELL_APP = (ROOT / "src/apps/solar_os_shell.c").read_text(encoding="utf-8")
GESTURE_COMPLETION = (
    ROOT / "src/shell/solar_os_shell_gesture_completion.c"
).read_text(encoding="utf-8")
MAIN = (ROOT / "src/main.c").read_text(encoding="utf-8")
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(encoding="utf-8")
JOB = (ROOT / "src/jobs/solar_os_gesture_listener_job.c").read_text(
    encoding="utf-8"
)
REGISTRY = (ROOT / "src/jobs/solar_os_job_registry.c").read_text(encoding="utf-8")


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
        self.assertIn("input_action_ensure_worker()", observer)
        self.assertIn("solar_os_input_action_matches", observer)

    def test_listener_does_not_keep_worker_stack_while_idle(self):
        start = ACTIONS.split("esp_err_t solar_os_input_actions_start", 1)[1].split(
            "void solar_os_input_actions_stop", 1
        )[0]
        self.assertIn("input_action_ensure_queue()", start)
        self.assertNotIn("input_action_ensure_worker()", start)
        worker = ACTIONS.split("static void input_action_worker", 1)[1].split(
            "static esp_err_t input_action_ensure_worker", 1
        )[0]
        self.assertIn("queue_empty", worker)
        self.assertIn("solar_os_task_delete_internal(NULL)", worker)
        self.assertIn("input_action_worker_state_t", ACTIONS)
        self.assertNotIn("worker_starting", ACTIONS)
        stop = ACTIONS.split("void solar_os_input_actions_stop", 1)[1].split(
            "bool solar_os_input_actions_running", 1
        )[0]
        self.assertIn("input_action_wait_worker_published()", stop)

    def test_queue_generation_prevents_reused_ids_from_running_stale_actions(self):
        self.assertIn("item.generation == state.generation", ACTIONS)
        clear = ACTIONS.split("size_t solar_os_input_actions_clear", 1)[1].split(
            "esp_err_t solar_os_input_actions_emit_key", 1
        )[0]
        self.assertIn("state.next_id = 1U", clear)
        self.assertIn("state.generation++", clear)

    def test_shell_keeps_emit_under_input_and_moves_binding_crud_to_gesture(self):
        self.assertIn('strcmp(argv[1], "emit")', SHELL_INPUT)
        self.assertIn("solar_os_shell_cmd_gesture", SHELL_INPUT)
        for subcommand in ('"bind"', '"bindings"', '"unbind"'):
            self.assertIn(subcommand, SHELL_INPUT)
        self.assertIn("solar_os_input_actions_emit_key", SHELL_INPUT)
        self.assertIn("solar_os_input_write_key_tap", ACTIONS)
        self.assertIn("solar_os_input_actions_bind", SHELL_INPUT)
        self.assertIn("solar_os_input_actions_clear", SHELL_INPUT)

    def test_listener_uses_the_job_lifecycle(self):
        self.assertIn('"gesture-listener"', JOB)
        self.assertIn("solar_os_input_actions_start()", JOB)
        self.assertIn("solar_os_input_actions_stop()", JOB)
        self.assertIn(".worker_stack_bytes = SOLAR_OS_INPUT_ACTION_WORKER_STACK", JOB)
        self.assertIn("solar_os_gesture_listener_job", REGISTRY)
        self.assertIn('members = ["core_runtime", "core_shell", "job_gesture_listener"]',
                      PACKAGES)

    def test_gesture_command_discovers_sources_and_drives_completion(self):
        self.assertIn("gesture_print_sources", SHELL_INPUT)
        self.assertIn("source.gesture_mask", SHELL_INPUT)
        self.assertIn("shell_complete_gesture_argument", SHELL_APP)
        self.assertIn("solar_os_shell_gesture_completion_emit", SHELL_APP)
        self.assertIn('"source=*"', GESTURE_COMPLETION)
        self.assertIn('"gesture=%s"', GESTURE_COMPLETION)
        self.assertIn('"shell/solar_os_shell_gesture_completion.c"', PACKAGES)
        self.assertIn('"services/solar_os_input_action_match.c"', PACKAGES)

    def test_runtime_wires_background_runner_and_package_source(self):
        self.assertIn("solar_os_input_actions_init()", MAIN)
        self.assertIn(
            "solar_os_input_actions_set_runner(solar_os_shell_run_background_command)",
            MAIN,
        )
        self.assertIn('"services/solar_os_input_actions.c"', PACKAGES)


if __name__ == "__main__":
    unittest.main()
