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
        self.assertIn("picotts_add(work->text", JOB_SOURCE)

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
        self.assertIn(
            '{"say", "queue offline speech", solar_os_shell_cmd_say}',
            SHELL_REGISTRY,
        )
        self.assertIn("solar_os_speech_enqueue(&request, &request_id)", SHELL_SOURCE)
        self.assertIn("--drop-if-busy", SHELL_SOURCE)
        self.assertIn("job start speechd", SHELL_SOURCE)
        self.assertIn("SHELL_COMPLETION_OPTIONS(path_say, say_options)", SHELL_REGISTRY)


if __name__ == "__main__":
    unittest.main()
