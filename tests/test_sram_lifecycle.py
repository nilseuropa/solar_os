"""Exercise production cold-state lifecycle functions with fault-injected providers."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8")


def function(text, name):
    match = re.search(r"(?:static\s+)?[\w\s*]+\b" + re.escape(name)
                      + r"\s*\([^;{}]*\)\s*\{", text)
    if match is None:
        raise AssertionError(f"missing function {name}")
    start = match.start()
    opening = text.index("{", match.start())
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + "\n"


def typedef(text, name):
    end = text.index("} " + name + ";") + len("} " + name + ";")
    start = text.rfind("typedef struct {", 0, end)
    return text[start:end] + "\n"


COMMON = r"""
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
typedef int esp_err_t;
typedef int BaseType_t;
typedef int solar_os_context_t;
typedef void *TaskHandle_t;
typedef unsigned TickType_t;
typedef int StaticSemaphore_t;
typedef void *SemaphoreHandle_t;
typedef int portMUX_TYPE;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_INVALID_ARG 4
#define ESP_ERR_TIMEOUT 5
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define EXT_RAM_BSS_ATTR
#define SOLAR_OS_MEMORY_EXTERNAL_PREFERRED 1
#define SOLAR_OS_MEMORY_INTERNAL_CRITICAL 2
#define SOLAR_OS_TASK_ROLE_BACKGROUND 1
#define SOLAR_OS_TASK_ROLE_SYSTEM 2
#define SOLAR_OS_TASK_STOP_WAIT_MS 100
#define SOLAR_OS_JOB_RESOURCE_NET 1
#define SOLAR_OS_JOB_RESOURCE_FILE 2
#define tskNO_AFFINITY 0
#define SOLAR_OS_LOGW(...) ((void)0)
#define SOLAR_OS_LOGI(...) ((void)0)
static const char *TAG __attribute__((unused)) = "test";
static int live, allocation_count, fail_alloc, last_class;
static size_t last_size;
static void *solar_os_memory_alloc(size_t size, int cls, const char *tag) {
    (void)tag; last_size = size; last_class = cls; allocation_count++;
    if (fail_alloc) return NULL;
    void *p = malloc(size); assert(p); live++; return p;
}
static void *solar_os_memory_calloc(size_t n, size_t size, int cls, const char *tag) {
    void *p = solar_os_memory_alloc(n * size, cls, tag);
    if (p) memset(p, 0, n * size); return p;
}
static void solar_os_memory_free(void *p) {
    if (p) { assert(live > 0); live--; free(p); }
}
static size_t strlcpy(char *out, const char *in, size_t n) {
    size_t len = strlen(in);
    if (n) { size_t copy = len < n - 1 ? len : n - 1;
        memcpy(out, in, copy); out[copy] = 0; }
    return len;
}
"""


class SramLifecycleTest(unittest.TestCase):
    def run_c(self, code):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / "lifecycle.c"
            binary = Path(directory) / "lifecycle"
            fixture.write_text(COMMON + code, encoding="utf-8")
            built = subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
                str(fixture), "-lm", "-o", str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_chatd_allocation_failures_restart_and_stop_timeout(self):
        text = source("src/jobs/solar_os_chatd_job.c")
        declarations = r"""
#define SOLAR_OS_CHAT_USER_MAX 32
#define SOLAR_OS_CHAT_DEVICE_MAX 32
#define SOLAR_OS_CHAT_CHANNEL_MAX 32
#define SOLAR_OS_CHAT_TEXT_MAX 128
#define SOLAR_OS_CHAT_TOKEN_MAX 64
#define SOLAR_OS_STORAGE_PATH_MAX 256
#define CHATD_HISTORY_TYPE_MAX 16
#define CHATD_MAX_CLIENTS 6
#define CHATD_MAX_CHANNELS 32
#define CHATD_DEFAULT_CHANNEL "main"
#define CHATD_DEFAULT_PORT 5555
#define CHATD_SC2 0
#define CHATD_TASK_STACK 8192
#define CHATD_TASK_PRIORITY 1
#define CHATD_STOP_WAIT_MS 100
#define SHUT_RDWR 2
"""
        declarations += "".join(typedef(text, name) for name in (
            "chatd_client_t", "chatd_history_entry_t", "chatd_job_state_t"))
        declarations += r"""
static chatd_job_state_t *chatd_state;
#define chatd_job (*chatd_state)
static const struct { const char *name; } solar_os_chatd_job = {"chatd"};
static int parse_error, buffer_error, listener_error, task_failure, worker_finishes;
static int shutdowns;
typedef struct { bool v1; bool sc2; const char *sc2_id; } chatd_opts_t;
static esp_err_t chatd_parse_args(int n, char **args, uint16_t *port,
                                 const char **token, const char **history, chatd_opts_t *opts) {
    (void)n; (void)args; (void)port; (void)token; (void)history; opts->v1 = true; return parse_error;
}
static esp_err_t chatd_alloc_buffers(chatd_job_state_t *s) {(void)s; return buffer_error;}
static void chatd_free_buffers(chatd_job_state_t *s) {(void)s;}
static void chatd_open_history_dump(chatd_job_state_t *s, const char *p) {(void)s; (void)p;}
static esp_err_t chatd_open_listener(chatd_job_state_t *s) {s->listen_fd = 9; return listener_error;}
static void chatd_close_all_sockets(chatd_job_state_t *s) {s->listen_fd = -1;}
static void chatd_job_task(void *p) {(void)p;}
static int solar_os_task_create_pinned_internal(void (*fn)(void *), const char *name,
    unsigned bytes, void *arg, int pri, TaskHandle_t *out, int core, int role) {
    (void)fn; (void)name; (void)bytes; (void)pri; (void)core; (void)role;
    assert(arg == chatd_state); assert(last_class == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED);
    if (task_failure) return 0; *out = (void *)1; return pdPASS;
}
static void solar_os_jobs_note_resource(const char *name, int kind, const char *p, const char *d) {
    (void)name; (void)kind; (void)p; (void)d;
}
static int shutdown(int fd, int how) {(void)fd; (void)how; shutdowns++; return 0;}
static void vTaskDelay(unsigned ticks) {
    (void)ticks;
    if (worker_finishes) { chatd_state->task = NULL; chatd_state->running = false; }
}
"""
        declarations += function(text, "chatd_job_start") + function(text, "chatd_job_stop")
        self.run_c(declarations + r"""
int main(void) {
    chatd_job_stop(NULL); assert(live == 0 && allocation_count == 0);
    parse_error = ESP_ERR_INVALID_ARG;
    assert(chatd_job_start(NULL, 0, NULL) == parse_error); assert(allocation_count == 0);
    parse_error = 0; fail_alloc = 1;
    assert(chatd_job_start(NULL, 0, NULL) == ESP_ERR_NO_MEM); assert(!chatd_state);
    fail_alloc = 0; buffer_error = ESP_ERR_NO_MEM;
    assert(chatd_job_start(NULL, 0, NULL) == buffer_error); assert(!chatd_state && live == 0);
    buffer_error = 0; listener_error = ESP_FAIL;
    assert(chatd_job_start(NULL, 0, NULL) == listener_error); assert(!chatd_state && live == 0);
    listener_error = 0; task_failure = 1;
    assert(chatd_job_start(NULL, 0, NULL) == ESP_ERR_NO_MEM); assert(!chatd_state && live == 0);
    task_failure = 0;
    assert(chatd_job_start(NULL, 0, NULL) == ESP_OK); assert(live == 1);
    chatd_job_stop(NULL); assert(live == 1 && chatd_state && shutdowns);
    assert(chatd_job_start(NULL, 0, NULL) == ESP_ERR_INVALID_STATE);
    /* Delayed worker completion: restarting reaps the retained old state. */
    chatd_state->task = NULL; chatd_state->running = false;
    assert(chatd_job_start(NULL, 0, NULL) == ESP_OK); assert(live == 1);
    worker_finishes = 1; chatd_job_stop(NULL); assert(!chatd_state && live == 0);
    chatd_job_stop(NULL); return 0;
}
""")

    def test_daq_status_retained_and_inflight_worker_not_freed(self):
        text = source("src/jobs/solar_os_daq_job.c")
        code = r"""
#define DAQ_MAX_STREAMS 4
#define SOLAR_OS_STORAGE_PATH_MAX 256
#define SOLAR_OS_STREAM_ID_MAX 32
#define DAQ_STREAM_LIST_MAX 128
#define SOLAR_OS_STREAM_CHANGE_KEY_MAX 128
typedef struct {int index;} solar_os_stream_handle_t;
typedef struct {char id[32]; int type;} solar_os_stream_info_t;
typedef int solar_os_event_t;
""" + typedef(text, "daq_job_state_t")
        code += typedef(source("src/jobs/solar_os_daq_job.h"), "solar_os_daq_status_t")
        # Header-owned maxima and stream enum type, kept compatible with this fixture.
        code = ("#define SOLAR_OS_DAQ_STREAM_ID_MAX 32\n"
                "#define SOLAR_OS_DAQ_STREAM_LIST_MAX 128\n"
                "typedef int solar_os_stream_type_t;\n" + code)
        code += r"""
static daq_job_state_t *daq_state;
#define daq (*daq_state)
static solar_os_daq_status_t daq_last_status;
static int start_result, stop_finishes, event_calls;
static bool daq_lock(void) {return true;}
static void daq_unlock(void) {}
static void daq_init_stream_handles(void) {}
static void daq_cleanup(void) {daq.running = false; daq.worker_task = NULL; daq.stream_count = 0;}
static esp_err_t daq_start_locked(solar_os_context_t *ctx, int n, char **args) {
    (void)ctx; (void)n; (void)args;
    if (daq.worker_task) return ESP_ERR_INVALID_STATE;
    daq.last_error = start_result;
    if (!start_result) {daq.running = true; daq.worker_task = (void *)1;
        strcpy(daq.path, "/data.csv"); daq.written_records = 17;}
    return start_result;
}
static void daq_stop_locked(solar_os_context_t *ctx) {
    (void)ctx; daq.running = false; if (stop_finishes) daq_cleanup();
}
static bool daq_event_locked(solar_os_context_t *ctx, const solar_os_event_t *event) {
    (void)ctx; (void)event; event_calls++; return daq.running;
}
"""
        code += "".join(function(text, n) for n in (
            "daq_copy_status", "daq_release_state", "daq_start", "daq_stop",
            "daq_event", "solar_os_daq_job_get_status"))
        self.run_c(code + r"""
int main(void) {
    solar_os_daq_status_t status;
    solar_os_daq_job_get_status(&status); assert(!status.running && allocation_count == 0);
    assert(!daq_event(NULL, NULL) && event_calls == 0); daq_stop(NULL);
    fail_alloc = 1;
    assert(daq_start(NULL, 0, NULL) == ESP_ERR_NO_MEM && !daq_state);
    solar_os_daq_job_get_status(&status); assert(status.last_error == ESP_ERR_NO_MEM);
    fail_alloc = 0; start_result = ESP_FAIL;
    assert(daq_start(NULL, 0, NULL) == ESP_FAIL && !daq_state && live == 0);
    start_result = ESP_OK;
    assert(daq_start(NULL, 0, NULL) == ESP_OK && live == 1);
    assert(last_class == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED);
    assert(daq_event(NULL, NULL) && event_calls == 1);
    daq_stop(NULL); assert(daq_state && live == 1);
    assert(daq_start(NULL, 0, NULL) == ESP_ERR_INVALID_STATE && live == 1);
    stop_finishes = 1; daq_stop(NULL); assert(!daq_state && live == 0);
    solar_os_daq_job_get_status(&status);
    assert(!status.running && status.written_records == 17);
    assert(!strcmp(status.path, "/data.csv"));
    daq_stop(NULL); return 0;
}
""")

    def test_voice_hot_copy_failure_timeout_and_persistent_configuration(self):
        text = source("src/services/solar_os_synth_voice.c")
        code = r"""
#define CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY 1
#define VOICE_BLOCK_FRAMES 256
#define SOLAR_OS_SYNTH_OWNER_MAX 24
typedef struct {bool claimed; char owner[24]; int voices[8], mono_held[16];
    int wavetable_marker, performance_marker;} voice_state_t;
static voice_state_t voice_idle, *voice_active;
#define voice_state (*(voice_active != NULL ? voice_active : &voice_idle))
typedef struct {const char *owner; void (*render)(void); void *user; size_t block_frames;} solar_os_synth_config_t;
typedef struct {bool running, starting; char owner[24]; unsigned sample_rate;} solar_os_synth_status_t;
static int start_result, stop_result, stop_calls;
static solar_os_synth_status_t publisher;
static esp_err_t voice_ensure_mutex(void) {return ESP_OK;}
static void voice_lock(void) {}
static void voice_unlock(void) {}
static bool voice_owner_matches(const char *owner) {return !strcmp(voice_state.owner, owner);}
static void voice_render(void) {}
static void voice_prepare_filter_table(unsigned rate) {(void)rate;}
static esp_err_t solar_os_synth_start(const solar_os_synth_config_t *config) {
    assert(voice_active && last_class == SOLAR_OS_MEMORY_INTERNAL_CRITICAL);
    if (start_result != ESP_ERR_INVALID_STATE) strcpy(publisher.owner, config->owner);
    if (!start_result) publisher.running = true;
    return start_result;
}
static esp_err_t solar_os_synth_stop(const char *owner) {
    assert(!strcmp(owner, publisher.owner)); stop_calls++;
    if (!stop_result) {publisher.running = false; publisher.owner[0] = 0;}
    return stop_result;
}
static void solar_os_synth_get_status(solar_os_synth_status_t *status) {*status = publisher;}
"""
        code += """static void voice_release_internal_locked(void);
""" + "".join(function(text, n) for n in (
            "voice_release_internal_locked", "voice_claim", "solar_os_synth_voice_stop"))
        self.run_c(code + r"""
int main(void) {
    voice_idle.wavetable_marker = 123; voice_idle.performance_marker = 456;
    fail_alloc = 1;
    assert(voice_claim("piano") == ESP_ERR_NO_MEM && !voice_active && !voice_idle.claimed);
    fail_alloc = 0; start_result = ESP_ERR_INVALID_STATE;
    strcpy(publisher.owner, "other"); publisher.running = true;
    assert(voice_claim("piano") == ESP_ERR_INVALID_STATE);
    assert(!voice_active && live == 0 && stop_calls == 0 && publisher.running);
    start_result = ESP_FAIL; stop_result = ESP_OK;
    assert(voice_claim("piano") == ESP_FAIL && !voice_active && live == 0);
    start_result = ESP_ERR_TIMEOUT; stop_result = ESP_ERR_TIMEOUT;
    assert(voice_claim("piano") == ESP_ERR_TIMEOUT && voice_active && live == 1);
    assert(voice_state.claimed);
    assert(solar_os_synth_voice_stop("other") == ESP_ERR_INVALID_STATE && live == 1);
    assert(solar_os_synth_voice_stop("piano") == ESP_ERR_TIMEOUT && live == 1);
    stop_result = ESP_OK;
    assert(solar_os_synth_voice_stop("piano") == ESP_OK && !voice_active && live == 0);
    assert(voice_idle.wavetable_marker == 123 && voice_idle.performance_marker == 456);
    start_result = ESP_OK;
    assert(voice_claim("piano") == ESP_OK && voice_active && live == 1);
    voice_state.wavetable_marker = 789;
    assert(voice_claim("other") == ESP_ERR_INVALID_STATE);
    assert(solar_os_synth_voice_stop("piano") == ESP_OK && live == 0);
    assert(voice_idle.wavetable_marker == 789 && !voice_idle.claimed);
    return 0;
}
""")

    def test_layout_members_not_released_until_callbacks_and_worker_stop(self):
        text = source("src/services/solar_os_display_layout.c")
        code = r"""
#define SOLAR_OS_DISPLAY_LAYOUT_MAX 3
#define SOLAR_OS_DISPLAY_LAYOUT_MEMBER_MAX 4
#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 24
#define SOLAR_OS_DISPLAY_TARGET_OWNER_MAX 32
typedef int solar_os_display_layout_kind_t;
typedef int solar_os_display_layout_axis_t;
typedef struct {int x, y, width, height;} solar_os_display_layout_rect_t;
typedef struct {char data[64];} u8g2_t;
typedef struct {int data;} u8x8_display_info_t;
typedef struct solar_os_display_layout_runtime solar_os_display_layout_runtime_t;
"""
        code += "".join(typedef(text, n) for n in (
            "display_layout_logical_t", "display_layout_backing_t", "display_layout_members_t"))
        beginning = text.index("struct solar_os_display_layout_runtime {")
        end = text.index("\n};", beginning) + 3
        code += text[beginning:end] + r"""
static solar_os_display_layout_runtime_t layouts[SOLAR_OS_DISPLAY_LAYOUT_MAX];
static portMUX_TYPE layouts_lock;
static int unregister_result, stop_result, release_calls;
static void vSemaphoreDelete(SemaphoreHandle_t p) {(void)p;}
static esp_err_t solar_os_display_unregister_targets(const char **names, size_t n) {
    (void)names; (void)n; return unregister_result;
}
static esp_err_t layout_stop_worker(solar_os_display_layout_runtime_t *layout) {
    if (!stop_result) layout->task = NULL; return stop_result;
}
static void layout_release_backing(solar_os_display_layout_runtime_t *layout) {
    (void)layout; release_calls++;
}
"""
        code += "".join(function(text, n) for n in (
            "layout_activate", "layout_reserve_create", "layout_free", "layout_rollback", "layout_destroy"))
        self.run_c(code + r"""
int main(void) {
    int slot = -1; fail_alloc = 1;
    assert(layout_reserve_create("canvas", &slot) == ESP_ERR_NO_MEM);
    assert(slot == -1 && !layouts[0].reserved && live == 0);
    fail_alloc = 0;
    assert(layout_reserve_create("canvas", &slot) == ESP_OK && slot == 0 && live == 1);
    assert(last_class == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED);
    int duplicate;
    assert(layout_reserve_create("canvas", &duplicate) == ESP_ERR_INVALID_STATE);
    solar_os_display_layout_runtime_t *layout = &layouts[slot];
    layout->logical_count = 1; layout->members->logical[0].registered = true;
    strcpy(layout->members->logical[0].name, "logical"); layout->task = (void *)1;
    unregister_result = ESP_FAIL;
    assert(layout_rollback(layout) == ESP_FAIL && live == 1 && layout->active);
    unregister_result = ESP_OK; stop_result = ESP_ERR_TIMEOUT;
    assert(layout_destroy(layout) == ESP_ERR_TIMEOUT && live == 1 && release_calls == 0);
    stop_result = ESP_OK;
    assert(layout_destroy(layout) == ESP_OK && live == 0 && release_calls == 1);
    assert(!layout->active && !layout->reserved && !layout->members);
    assert(layout_reserve_create("canvas", &slot) == ESP_OK && live == 1);
    assert(layout_rollback(&layouts[slot]) == ESP_OK && live == 0);
    return 0;
}
""")

    def test_synth_pcm_sized_on_start_freed_on_worker_exit_not_stop_timeout(self):
        text = source("src/services/solar_os_synth.c")
        code = r"""
#define SOLAR_OS_SYNTH_OWNER_MAX 24
#define SOLAR_OS_STREAM_ID_MAX 32
#define SOLAR_OS_SYNTH_BLOCK_FRAMES_MIN 32
#define SOLAR_OS_SYNTH_BLOCK_FRAMES_MAX 512
#define SYNTH_TASK_STACK 5120
#define SYNTH_TASK_PRIORITY 1
#define SYNTH_START_WAIT_MS 2000
typedef void (*solar_os_synth_render_cb_t)(int16_t *, size_t, uint32_t, void *);
"""
        header = source("src/services/solar_os_synth.h")
        code += typedef(header, "solar_os_synth_config_t") + typedef(header, "solar_os_synth_status_t")
        code += typedef(text, "synth_state_t") + r"""
static synth_state_t synth;
static int task_failure, start_signalled = 1, worker_finishes;
static esp_err_t synth_ensure_sync(void) {synth.mutex = (void *)1; synth.started = (void *)2; return ESP_OK;}
static void synth_lock(void) {}
static void synth_unlock(void) {}
static TaskHandle_t xTaskGetCurrentTaskHandle(void) {return NULL;}
static int xSemaphoreTake(SemaphoreHandle_t sem, unsigned timeout) {
    return sem == synth.started ? timeout != 0 && start_signalled : pdTRUE;
}
static void synth_worker(void *arg) {(void)arg;}
static void synth_finish(esp_err_t result);
static bool solar_os_task_wait_done(TaskHandle_t task, volatile bool *done, unsigned ms) {
    (void)task; (void)ms;
    if (!worker_finishes) return false;
    synth_finish(ESP_OK); *done = true; return true;
}
static int solar_os_task_create_pinned_internal(void (*fn)(void *), const char *name,
    unsigned bytes, void *arg, int pri, TaskHandle_t *out, int core, int role) {
    (void)fn; (void)name; (void)bytes; (void)arg; (void)pri; (void)core; (void)role;
    if (task_failure) return 0;
    *out = (void *)3; synth.start_result = ESP_OK;
    synth.starting = false; synth.running = true; return pdPASS;
}
esp_err_t solar_os_synth_stop(const char *owner);
""" + "".join(function(text, n) for n in (
            "synth_finish", "solar_os_synth_start", "solar_os_synth_stop", "solar_os_synth_get_status"))
        self.run_c(code + r"""
static void render(int16_t *p, size_t n, uint32_t rate, void *user) {
    (void)p; (void)n; (void)rate; (void)user;
}
int main(void) {
    solar_os_synth_status_t status;
    solar_os_synth_get_status(&status); assert(allocation_count == 0 && !status.running);
    solar_os_synth_config_t config = {.owner = "osc", .render = render, .block_frames = 128};
    fail_alloc = 1;
    assert(solar_os_synth_start(&config) == ESP_ERR_NO_MEM && live == 0 && !synth.samples);
    fail_alloc = 0; task_failure = 1;
    assert(solar_os_synth_start(&config) == ESP_ERR_NO_MEM && live == 0 && !synth.samples);
    task_failure = 0; start_signalled = 0;
    assert(solar_os_synth_start(&config) == ESP_ERR_TIMEOUT && live == 1 && synth.samples);
    assert(last_size == 128 * 2 * sizeof(int16_t) && last_class == SOLAR_OS_MEMORY_INTERNAL_CRITICAL);
    assert(solar_os_synth_stop("other") == ESP_ERR_INVALID_STATE && live == 1);
    assert(solar_os_synth_stop("osc") == ESP_ERR_TIMEOUT && live == 1);
    worker_finishes = 1;
    assert(solar_os_synth_stop("osc") == ESP_OK && live == 0 && !synth.samples);
    start_signalled = 1;
    assert(solar_os_synth_start(&config) == ESP_OK && live == 1);
    assert(solar_os_synth_start(&config) == ESP_ERR_INVALID_STATE && live == 1);
    assert(solar_os_synth_stop("osc") == ESP_OK && live == 0);
    solar_os_synth_get_status(&status); assert(!status.running); return 0;
}
""")

    def test_real_voice_renderer_with_and_without_external_bss(self):
        # Keep production structs, math and renderer intact; substitute only
        # the external synth, DSP-level and FreeRTOS providers.
        def without_includes(text):
            return re.sub(r'^\s*#(?:include|pragma once).*$', '', text, flags=re.M)

        headers = "#include <limits.h>\n#include <math.h>\n#define SOLAR_OS_STREAM_ID_MAX 32\n"
        for path in ("src/services/solar_os_synth.h", "src/services/solar_os_synth_voice.h",
                     "src/services/solar_os_dsp.h"):
            headers += without_includes(source(path))
        providers = r"""
static solar_os_synth_status_t publisher;
static solar_os_synth_render_cb_t render_callback;
static void *render_user;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *p) {return p;}
static int xSemaphoreTake(SemaphoreHandle_t sem, unsigned ticks) {(void)sem; (void)ticks; return pdTRUE;}
static int xSemaphoreGive(SemaphoreHandle_t sem) {(void)sem; return pdTRUE;}
esp_err_t solar_os_synth_start(const solar_os_synth_config_t *config) {
    render_callback = config->render; render_user = config->user;
    strlcpy(publisher.owner, config->owner, sizeof(publisher.owner));
    publisher.running = true; publisher.sample_rate = 16000; return ESP_OK;
}
esp_err_t solar_os_synth_stop(const char *owner) {
    assert(!strcmp(owner, publisher.owner)); publisher.running = false;
    publisher.owner[0] = 0; render_callback = NULL; return ESP_OK;
}
void solar_os_synth_get_status(solar_os_synth_status_t *status) {*status = publisher;}
esp_err_t solar_os_dsp_level_s16(const int16_t *p, size_t n, solar_os_dsp_level_t *level) {
    uint32_t peak = 0;
    for (size_t i = 0; i < n; i++) {int v = abs(p[i]); if ((uint32_t)v > peak) peak = v;}
    level->peak = peak; level->rms = peak / 2; return ESP_OK;
}
"""
        main = r"""
int main(void) {
    solar_os_synth_voice_status_t status;
    solar_os_synth_voice_get_status(&status); assert(live == 0 && !status.running);
    solar_os_synth_voice_config_t config = status.config;
    config.waveform = SOLAR_OS_SYNTH_WAVE_CUSTOM;
    config.attack_ms = 0; config.decay_ms = 0; config.sustain_percent = 100;
    config.filter.cutoff_hz = 1700; config.filter.resonance_percent = 25;
    assert(solar_os_synth_voice_configure("test", &config) == ESP_OK);
    solar_os_synth_voice_performance_t performance = {.mono = true, .glide_ms = 40};
    assert(solar_os_synth_voice_configure_performance("test", &performance) == ESP_OK);
    int16_t table[256], returned[256];
    for (size_t i = 0; i < 256; i++) table[i] = (int16_t)(i * 200 - 25000);
    assert(solar_os_synth_voice_set_wavetable("test", table, 256) == ESP_OK);
    assert(live == 0); /* Configuring and inspecting do not consume hot SRAM. */
    assert(solar_os_synth_voice_note_on("test", 440, 100) == ESP_OK);
#if CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
    assert(live == 1 && voice_active && last_class == SOLAR_OS_MEMORY_INTERNAL_CRITICAL);
#else
    assert(live == 0 && !voice_active); /* No duplicate state on SRAM-only builds. */
#endif
    int16_t first[512] = {0}, second[512] = {0};
    render_callback(first, 256, 16000, render_user);
    solar_os_synth_voice_get_status(&status);
    assert(status.running && status.active_voices == 1 && status.pcm_peak > 0);
    assert(solar_os_synth_voice_stop("test") == ESP_OK && live == 0 && !voice_active);
    assert(solar_os_synth_voice_get_wavetable(returned, 256) == ESP_OK);
    assert(!memcmp(table, returned, sizeof(table)));
    solar_os_synth_voice_get_status(&status);
    assert(!status.running && status.config.filter.cutoff_hz == 1700 && status.performance.mono);
    assert(solar_os_synth_voice_note_on("test", 440, 100) == ESP_OK);
    render_callback(second, 256, 16000, render_user);
    assert(!memcmp(first, second, sizeof(first)));
    assert(solar_os_synth_voice_note_on("test", 660, 100) == ESP_OK);
    assert(solar_os_synth_voice_note_off("test", 660) == ESP_OK);
    assert(solar_os_synth_voice_all_notes_off("test") == ESP_OK);
    assert(solar_os_synth_voice_stop("test") == ESP_OK && live == 0);
    return 0;
}
"""
        real = without_includes(source("src/services/solar_os_synth_voice.c"))
        for enabled in (1, 0):
            with self.subTest(external_bss=enabled):
                self.run_c(f"#define CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY {enabled}\n"
                           + headers + providers + real + main)

    def test_provider_storage_internal_and_telnet_release_after_client_cleanup(self):
        for path, declaration in (
            ("src/services/solar_os_buses.c", "static StaticSemaphore_t bus_mutex_buffers"),
            ("src/services/solar_os_synth_voice.c", "static StaticSemaphore_t voice_mutex_storage"),
            ("src/services/solar_os_synth.c", "static StaticSemaphore_t synth_mutex_storage"),
            ("src/jobs/solar_os_daq_job.c", "static StaticSemaphore_t daq_mutex_storage"),
        ):
            self.assertIn(declaration, source(path))
        telnet = source("src/jobs/solar_os_telnetd_job.c")
        worker = function(telnet, "telnetd_job_task")
        self.assertLess(worker.index("while (!telnetd_cleanup_client())"),
                        worker.index("solar_os_memory_free(state->payload)"))
        self.assertLess(worker.index("solar_os_memory_free(state->payload)"),
                        worker.index("state->task = NULL"))
        self.assertNotIn("solar_os_memory_free", function(telnet, "telnetd_job_stop"))
        voice = source("src/services/solar_os_synth_voice.c")
        self.assertNotIn("solar_os_memory_alloc", function(voice, "solar_os_synth_voice_get_status"))
        self.assertIn("CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY", function(voice, "voice_claim"))


if __name__ == "__main__":
    unittest.main()
