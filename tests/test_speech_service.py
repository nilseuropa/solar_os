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

    def test_streaming_request_uses_one_bounded_chunk_slot(self):
        self.assertIn("solar_os_speech_stream_begin", SERVICE_HEADER)
        self.assertIn("solar_os_speech_stream_write", SERVICE_HEADER)
        self.assertIn(
            "char stream_chunk[SOLAR_OS_SPEECH_TEXT_MAX + 1U]", SERVICE_SOURCE
        )
        stream_write = SERVICE_SOURCE.split(
            "esp_err_t solar_os_speech_stream_write(", 1
        )[1].split("esp_err_t solar_os_speech_cancel(", 1)[0]
        self.assertIn("speech.stream_chunk_ready", stream_write)
        self.assertIn("speech.stream_final_submitted", stream_write)
        self.assertIn("xTaskNotifyGive(worker)", stream_write)

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

    def test_speechd_keeps_one_player_open_for_a_stream(self):
        run = JOB_SOURCE.split("static void speechd_run_stream(", 1)[1]
        run = run.split("static void speechd_task(", 1)[0]
        self.assertIn("solar_os_speech_worker_stream_take(", run)
        self.assertIn("picotts_stream_begin(", run)
        self.assertIn("picotts_stream_write(", run)
        self.assertIn("picotts_stream_end(", run)
        self.assertEqual(run.count("speechd_open_player(work)"), 1)
        self.assertEqual(run.count("speechd_close_player(cancelled)"), 1)
        self.assertLess(
            run.index("speechd_open_player(work)"),
            run.index("while (stream_open"),
        )
        self.assertLess(
            run.index("while (stream_open"),
            run.index("speechd_close_player(cancelled)"),
        )

    def test_picotts_input_wait_is_bounded_and_cancellable(self):
        self.assertIn("PICOTTS_INPUT_QUEUE_SIZE=576", PICOTTS_CMAKE)
        self.assertIn("const volatile bool *cancelled", PICOTTS_HEADER)
        self.assertIn(
            "xQueueCreate(PICOTTS_INPUT_QUEUE_SIZE, sizeof(uint16_t))",
            PICOTTS_RUNTIME,
        )
        enqueue = PICOTTS_RUNTIME.split("static bool pico_queue_item(", 1)[1]
        enqueue = enqueue.split("static bool pico_queue_bytes(", 1)[0]
        self.assertIn("INPUT_QUEUE_WAIT_MS", enqueue)
        self.assertIn("*cancelled", enqueue)
        self.assertNotIn("portMAX_DELAY", enqueue)

    def test_picotts_drains_output_when_its_input_buffer_is_full(self):
        task = PICOTTS_RUNTIME.split("static void pico_task_main(", 1)[1]
        task = task.split("static bool pico_cleanup(", 1)[0]
        self.assertIn("pico_control_flags(", task)
        self.assertIn("PICOTASK_EXIT", task)
        self.assertIn("Pico's input buffer is full", task)
        self.assertIn("utterance_end_seen", task)
        self.assertIn("idle_callback();", task)

    def test_picotts_abort_resets_queued_and_internal_engine_state(self):
        self.assertIn("bool picotts_abort(void)", PICOTTS_HEADER)
        task = PICOTTS_RUNTIME.split("static void pico_task_main(", 1)[1]
        task = task.split("static bool pico_cleanup(", 1)[0]
        self.assertIn("PICOTASK_ABORT", task)
        self.assertIn("xQueueReset(text_queue)", task)
        self.assertIn(
            "pico_resetEngine(pico_engine, PICO_RESET_SOFT)", task
        )
        abort = PICOTTS_RUNTIME.split("bool picotts_abort(void)", 1)[1]
        abort = abort.split("bool picotts_shutdown(void)", 1)[0]
        self.assertIn("ABORT_WAIT_MS", abort)
        self.assertNotIn("portMAX_DELAY", abort)

    def test_speechd_aborts_cancelled_synthesis_instead_of_draining_it(self):
        abort = JOB_SOURCE.split("static bool speechd_abort_engine(", 1)[1]
        abort = abort.split("static void speechd_run_request(", 1)[0]
        self.assertIn("picotts_abort()", abort)
        stream = JOB_SOURCE.split("static void speechd_run_stream(", 1)[1]
        stream = stream.split("static void speechd_task(", 1)[0]
        self.assertIn("speechd_wait_engine_idle(cancelled)", stream)
        self.assertIn("speechd_abort_engine()", stream)
        self.assertNotIn("picotts_stream_end(NULL)", stream)

    def test_picotts_yields_during_synthesis(self):
        task = PICOTTS_RUNTIME.split("static void pico_task_main(", 1)[1]
        task = task.split("static bool pico_cleanup(", 1)[0]
        self.assertIn("INPUT_COOPERATIVE_BYTES", task)
        self.assertIn("OUTPUT_COOPERATIVE_STEPS", task)
        self.assertGreaterEqual(task.count("vTaskDelay(1);"), 2)

    def test_picotts_reports_completed_synthesis_input(self):
        self.assertIn("picotts_progress_notify_fn", PICOTTS_HEADER)
        self.assertIn("QUEUE_SEGMENT_END", PICOTTS_RUNTIME)
        self.assertIn("if (segment_end_seen)", PICOTTS_RUNTIME)
        self.assertIn("pico_progress_report();", PICOTTS_RUNTIME)
        self.assertNotIn("PROGRESS_SEGMENT_BYTES", PICOTTS_RUNTIME)
        self.assertIn("picotts_set_progress_notify(speechd_progress)", JOB_SOURCE)
        self.assertIn("solar_os_speech_worker_set_progress(", JOB_SOURCE)

    def test_picotts_stream_only_terminates_on_final_chunk(self):
        self.assertIn("picotts_stream_begin", PICOTTS_HEADER)
        self.assertIn("picotts_stream_write", PICOTTS_HEADER)
        stream_write = PICOTTS_RUNTIME.split(
            "static bool pico_stream_write(", 1
        )[1].split("bool picotts_add(", 1)[0]
        self.assertIn("if (!final)", stream_write)
        self.assertIn("QUEUE_SEGMENT_END | QUEUE_UTTERANCE_END", stream_write)

    def test_pitch_and_speed_flow_through_speech_api(self):
        self.assertIn("SOLAR_OS_SPEECH_PITCH_MIN 50U", SERVICE_HEADER)
        self.assertIn("SOLAR_OS_SPEECH_PITCH_MAX 200U", SERVICE_HEADER)
        self.assertIn("SOLAR_OS_SPEECH_SPEED_MIN 20U", SERVICE_HEADER)
        self.assertIn("SOLAR_OS_SPEECH_SPEED_MAX 500U", SERVICE_HEADER)
        self.assertIn("request->pitch", SERVICE_SOURCE)
        self.assertIn("request->speed", SERVICE_SOURCE)
        self.assertIn("work->pitch", JOB_SOURCE)
        self.assertIn("work->speed", JOB_SOURCE)
        self.assertIn('"<pitch level=', PICOTTS_RUNTIME)
        self.assertIn('<speed level=', PICOTTS_RUNTIME)
        self.assertIn("solaros_speech_say_obj, 1, 5", PYTHON_SOURCE)
        for source in (PYTHON_SOURCE, LUA_SOURCE):
            self.assertIn("SOLAR_OS_SPEECH_PITCH_DEFAULT", source)
            self.assertIn("SOLAR_OS_SPEECH_SPEED_DEFAULT", source)

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
        for source in (PYTHON_SOURCE, LUA_SOURCE):
            self.assertIn('"progress_done"', source)
            self.assertIn('"progress_total"', source)

    def test_say_command_queues_copied_text(self):
        speechd = self.catalog.package_defs["job_speechd"]
        self.assertIn("shell/solar_os_shell_speech.c", speechd.sources)
        self.assertIn('"say", "speak text or a text file"', SHELL_REGISTRY)
        self.assertIn("solar_os_speech_enqueue(&request, request_id)", SHELL_SOURCE)
        self.assertIn("--drop-if-busy", SHELL_SOURCE)
        self.assertIn('strcmp(arg, "--pitch") == 0', SHELL_SOURCE)
        self.assertIn('strcmp(arg, "--speed") == 0', SHELL_SOURCE)
        self.assertIn("job start speechd", SHELL_SOURCE)
        self.assertIn("SHELL_COMPLETION_OPTIONS(path_say, say_options)", SHELL_REGISTRY)
        self.assertIn("SHELL_COMPLETION_STATIC(path_say_pitch", SHELL_REGISTRY)
        self.assertIn("SHELL_COMPLETION_STATIC(path_say_speed", SHELL_REGISTRY)

    def test_say_reads_plain_text_files_with_progress_and_cancellation(self):
        self.assertIn('strcmp(arg, "--file") == 0', SHELL_SOURCE)
        self.assertIn("say_validate_plain_text(text, chunk)", SHELL_SOURCE)
        self.assertNotIn("say_validate_plain_text(file)", SHELL_SOURCE)
        self.assertIn("say_render_progress", SHELL_SOURCE)
        self.assertIn("bytes_done * 10000U", SHELL_SOURCE)
        self.assertIn('"] %3u.%02u%%"', SHELL_SOURCE)
        self.assertIn("bytes_submitted", SHELL_SOURCE)
        self.assertNotIn("status.progress_done", SHELL_SOURCE)
        self.assertNotIn("status.progress_total", SHELL_SOURCE)
        self.assertIn("solar_os_speech_stream_begin(", SHELL_SOURCE)
        enqueue = SHELL_SOURCE.split(
            "const esp_err_t write_err = solar_os_speech_stream_write(", 1
        )[1].split("bool solar_os_shell_speech_file_event", 1)[0]
        self.assertLess(
            enqueue.index("say_file_playback.bytes_submitted ="),
            enqueue.index("say_render_progress("),
        )
        self.assertNotIn("activity_phase", SHELL_SOURCE)
        self.assertIn("solar_os_speech_request_status", SHELL_SOURCE)
        self.assertIn(
            "solar_os_speech_cancel(say_file_playback.request_id)", SHELL_SOURCE
        )
        self.assertIn("SOLAR_OS_KEY_ESCAPE", SHELL_SOURCE)
        self.assertIn("ch == 0x03U", SHELL_SOURCE)
        self.assertIn("say_file_playback.stopping = true", SHELL_SOURCE)
        stop_handler = SHELL_SOURCE.split(
            "if (ch == SOLAR_OS_KEY_ESCAPE", 1
        )[1].split("if (event->type == SOLAR_OS_EVENT_TICK)", 1)[0]
        self.assertNotIn("say_file_finish(ctx", stop_handler)
        self.assertIn("if (say_file_playback.stopping)", SHELL_SOURCE)
        self.assertIn("solar_os_shell_speech_file_event", SHELL_SOURCE)
        self.assertIn("event->type == SOLAR_OS_EVENT_TICK", SHELL_SOURCE)
        self.assertIn("solar_os_shell_session_hold_prompt(ctx)", SHELL_SOURCE)
        self.assertNotIn("say_wait_for_request", SHELL_SOURCE)
        self.assertIn(
            "solar_os_shell_speech_file_event(ctx, event)", SHELL_REGISTRY
        )
        self.assertIn(
            "solar_os_shell_speech_file_session_destroyed(session)",
            SHELL_REGISTRY,
        )
        self.assertNotIn("SAY_FILE_DEFAULT_MAX_BYTES", SHELL_SOURCE)
        self.assertNotIn('strcmp(arg, "--force") == 0', SHELL_SOURCE)
        say_options = SHELL_REGISTRY.split(
            "static const char * const say_options[]", 1
        )[1].split("};", 1)[0]
        self.assertNotIn('"--force"', say_options)
        self.assertIn('"--file"', SHELL_REGISTRY)
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
