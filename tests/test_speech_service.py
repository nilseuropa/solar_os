import importlib.util
from pathlib import Path
import sys
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
GENERATOR_PATH = REPOSITORY / "scripts" / "generate_flavor_config.py"
SPEC = importlib.util.spec_from_file_location("speech_flavor_config", GENERATOR_PATH)
assert SPEC is not None and SPEC.loader is not None
generate_flavor_config = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = generate_flavor_config
SPEC.loader.exec_module(generate_flavor_config)

SERVICE_HEADER = (REPOSITORY / "src/services/solar_os_speech.h").read_text(
    encoding="utf-8"
)
SERVICE_SOURCE = (REPOSITORY / "src/services/solar_os_speech.c").read_text(
    encoding="utf-8"
)
JOB_SOURCE = (REPOSITORY / "src/jobs/solar_os_speechd_job.c").read_text(
    encoding="utf-8"
)
SHELL_SOURCE = (REPOSITORY / "src/shell/solar_os_shell_speech.c").read_text(
    encoding="utf-8"
)
SHELL_REGISTRY = (REPOSITORY / "src/apps/solar_os_shell.c").read_text(
    encoding="utf-8"
)
API_DESCRIPTOR = (REPOSITORY / "src/apps/solar_os_script_api.inc").read_text(
    encoding="utf-8"
)
PYTHON_SOURCE = (REPOSITORY / "src/apps/solar_os_python.c").read_text(
    encoding="utf-8"
)
LUA_SOURCE = (REPOSITORY / "src/apps/solar_os_lua.c").read_text(
    encoding="utf-8"
)
PICOTTS_CMAKE = (REPOSITORY / "components/picotts/CMakeLists.txt").read_text(
    encoding="utf-8"
)
PICOTTS_HEADER = (
    REPOSITORY / "components/picotts/include/picotts.h"
).read_text(encoding="utf-8")
PICOTTS_RUNTIME = (
    REPOSITORY / "components/picotts/picotts_runtime.c"
).read_text(encoding="utf-8")
PICOTTS_VOICES = REPOSITORY / "picotts_voices"


class SpeechServiceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalog = generate_flavor_config.load_catalog(
            REPOSITORY / "packages" / "solar_os_packages.toml"
        )

    def test_full_enables_optional_speech_job_and_dependencies(self):
        _, _, groups, packages = generate_flavor_config.load_flavor(
            REPOSITORY / "flavors/full.toml", self.catalog
        )
        self.assertTrue(groups["speech"])
        self.assertTrue(packages["job_speechd"])
        self.assertTrue(packages["service_speech"])
        self.assertEqual(
            self.catalog.package_defs["job_speechd"].requires, ("picotts",)
        )
        self.assertEqual(
            set(self.catalog.package_defs["job_speechd"].capabilities),
            {"audio", "psram"},
        )

    def test_queue_is_bounded_and_copies_text(self):
        self.assertIn("SOLAR_OS_SPEECH_TEXT_MAX 512U", SERVICE_HEADER)
        self.assertIn("SOLAR_OS_SPEECH_QUEUE_CAPACITY 8U", SERVICE_HEADER)
        enqueue = SERVICE_SOURCE.split("esp_err_t solar_os_speech_enqueue(", 1)[1]
        enqueue = enqueue.split("esp_err_t solar_os_speech_cancel(", 1)[0]
        self.assertIn("memcpy(entry->text, request->text, request->text_len)", enqueue)
        self.assertIn("speech.queue_count >= SOLAR_OS_SPEECH_QUEUE_CAPACITY", enqueue)
        self.assertIn("xTaskNotifyGive(worker)", enqueue)

    def test_job_uses_selected_audio_player_and_pcm_converter(self):
        self.assertIn('.owner = "job:speechd"', JOB_SOURCE)
        self.assertIn("solar_os_audio_player_create(", JOB_SOURCE)
        self.assertIn("solar_os_audio_s16_convert(", JOB_SOURCE)
        self.assertIn("solar_os_audio_player_write(", JOB_SOURCE)
        self.assertIn("solar_os_audio_player_destroy(", JOB_SOURCE)
        self.assertIn("picotts_add(", JOB_SOURCE)
        run = JOB_SOURCE.split("static void speechd_run_request(", 1)[1]
        run = run.split("static void speechd_task(", 1)[0]
        self.assertLess(
            run.index("speechd.awaiting_idle = true"),
            run.index("picotts_add("),
        )

    def test_picotts_input_wait_is_bounded_and_cancellable(self):
        self.assertIn("PICOTTS_INPUT_QUEUE_SIZE=513", PICOTTS_CMAKE)
        self.assertIn("const volatile bool *cancelled", PICOTTS_HEADER)
        add = PICOTTS_RUNTIME.split("bool picotts_add(", 1)[1]
        add = add.split("bool picotts_shutdown(", 1)[0]
        self.assertIn("INPUT_QUEUE_WAIT_MS", add)
        self.assertIn("*cancelled", add)
        self.assertNotIn("portMAX_DELAY", add)

    def test_picotts_drains_output_when_its_input_buffer_is_full(self):
        task = PICOTTS_RUNTIME.split("static void pico_task_main(", 1)[1]
        task = task.split("static bool pico_cleanup(", 1)[0]
        self.assertIn("if (pico_exit_requested())", task)
        self.assertIn("Pico's input buffer is full", task)
        self.assertIn("utterance_end_seen", task)
        self.assertIn("idle_callback();", task)

    def test_picotts_shutdown_wait_is_bounded(self):
        cleanup = PICOTTS_RUNTIME.split("static bool pico_cleanup(", 1)[1]
        cleanup = cleanup.split("bool picotts_init_resources(", 1)[0]
        self.assertIn("EXIT_WAIT_MS", cleanup)
        self.assertNotIn("portMAX_DELAY", cleanup)

    def test_speechd_stop_never_frees_a_running_engine(self):
        stop = JOB_SOURCE.split("static void speechd_stop(", 1)[1]
        stop = stop.split("static void speechd_detail(", 1)[0]
        self.assertIn("resources retained safely", stop)
        self.assertNotIn("forcing PicoTTS shutdown", stop)
        self.assertNotIn("while (!speechd.done)", stop)

    def test_job_loads_runtime_voice_directory(self):
        self.assertIn("usage: job start speechd <voice-directory>", JOB_SOURCE)
        self.assertIn('SPEECHD_TA_FILENAME "ta.bin"', JOB_SOURCE)
        self.assertIn('SPEECHD_SG_FILENAME "sg.bin"', JOB_SOURCE)
        self.assertIn("SOLAR_OS_MEMORY_EXTERNAL_REQUIRED", JOB_SOURCE)
        self.assertIn("picotts_init_resources(", JOB_SOURCE)
        self.assertIn("speechd_release_voice_resources();", JOB_SOURCE)

    def test_picotts_voice_blobs_are_versioned_external_assets(self):
        self.assertIn("picotts_init_resources", PICOTTS_HEADER)
        self.assertIn("solar_os_pico_load_resource(", PICOTTS_RUNTIME)
        self.assertNotIn("target_add_binary_data", PICOTTS_CMAKE)
        self.assertNotIn("picotts_ta.bin.S", PICOTTS_CMAKE)
        for locale in ("en-GB", "en-US", "de-DE", "es-ES", "fr-FR", "it-IT"):
            self.assertIn(f"solar_os_picotts_voice({locale})", PICOTTS_CMAKE)
            for resource in ("ta.bin", "sg.bin"):
                path = PICOTTS_VOICES / locale / resource
                self.assertTrue(path.is_file(), path)
                self.assertGreater(path.stat().st_size, 100_000, path)

    def test_python_and_lua_share_speech_surface(self):
        for function in ("say", "cancel", "request_status", "queue_status"):
            self.assertIn(
                f"SOLAR_OS_SCRIPT_API_FUNCTION(speech, {function}, {function});",
                API_DESCRIPTOR,
            )
            self.assertIn(f"solaros_speech_{function}", PYTHON_SOURCE)
            self.assertIn(f"solua_speech_{function}", LUA_SOURCE)

    def test_say_command_queues_copied_text(self):
        speechd = self.catalog.package_defs["job_speechd"]
        self.assertIn("shell/solar_os_shell_speech.c", speechd.sources)
        self.assertIn('"say", "speak text or a text file"', SHELL_REGISTRY)
        self.assertIn("solar_os_speech_enqueue(&request, request_id)", SHELL_SOURCE)
        self.assertIn("--drop-if-busy", SHELL_SOURCE)
        self.assertIn("job start speechd", SHELL_SOURCE)
        self.assertIn("SHELL_COMPLETION_OPTIONS(path_say, say_options)", SHELL_REGISTRY)

    def test_say_reads_plain_text_files_with_progress_and_cancellation(self):
        self.assertIn('strcmp(arg, "--file") == 0', SHELL_SOURCE)
        self.assertIn("say_validate_plain_text(text, chunk)", SHELL_SOURCE)
        self.assertNotIn("say_validate_plain_text(file)", SHELL_SOURCE)
        self.assertIn("say_render_progress", SHELL_SOURCE)
        self.assertIn("bytes_done * 10000U", SHELL_SOURCE)
        self.assertIn('"] %3u.%02u%%"', SHELL_SOURCE)
        self.assertIn("i == filled ? '>' : '-'", SHELL_SOURCE)
        self.assertIn("solar_os_speech_request_status", SHELL_SOURCE)
        self.assertIn("solar_os_speech_cancel(request_id)", SHELL_SOURCE)
        self.assertIn("SOLAR_OS_KEY_ESCAPE", SHELL_SOURCE)
        self.assertIn("ch == 0x03U", SHELL_SOURCE)
        self.assertIn("SAY_FILE_DEFAULT_MAX_BYTES", SHELL_SOURCE)
        self.assertIn("use --force to read it anyway", SHELL_SOURCE)
        self.assertIn('strcmp(arg, "--force") == 0', SHELL_SOURCE)
        self.assertIn('"--force", "--file"', SHELL_REGISTRY)
        self.assertIn(
            "SHELL_COMPLETION_PATH(path_say_file, false)",
            SHELL_REGISTRY,
        )

    def test_speechd_voice_directory_completion_lists_directories(self):
        self.assertIn(
            'path_job_start_speechd[] = {\n    "job", "start", "speechd"',
            SHELL_REGISTRY,
        )
        self.assertIn(
            "SHELL_COMPLETION_PATH(path_job_start_speechd, true)",
            SHELL_REGISTRY,
        )


if __name__ == "__main__":
    unittest.main()
