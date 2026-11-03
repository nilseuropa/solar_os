from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class RuntimeDeadlineTest(unittest.TestCase):
    def test_mixed_session_and_job_phases_and_lifecycle_filters(self):
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "solar_os_scheduler.h"
#define SOLAR_OS_SESSION_MAX 3
#define SOLAR_OS_JOB_RUNNING 1
static unsigned jobs_lock;
#define portENTER_CRITICAL(p) assert(!(*(p))++)
#define portEXIT_CRITICAL(p) assert((*(p))-- == 1)
typedef struct { void (*event)(void); uint32_t interval; } solar_os_app_t;
typedef struct { bool used, suspended; const solar_os_app_t *app; solar_os_tick_stats_t tick_stats; } solar_os_session_entry_t;
static struct {
    solar_os_session_entry_t sessions[3], *foreground_session;
    const solar_os_app_t *foreground_app, *legacy_tick_app;
    solar_os_tick_stats_t legacy_tick_stats;
} session_state;
static uint32_t solar_os_app_tick_interval_ms(const solar_os_app_t *app, uint32_t fallback)
{ return app->interval ? app->interval : fallback; }
typedef struct { void (*event)(void), (*event_with_user)(void); uint32_t tick_interval_ms; } solar_os_job_t;
typedef struct { const solar_os_job_t *job; } job_entry_t;
typedef struct { int state; bool lifecycle_busy; const job_entry_t *entry; solar_os_tick_stats_t tick_stats; } solar_os_job_runtime_t;
static solar_os_job_runtime_t job_runtimes[3];
static size_t job_runtime_count = 3;
int64_t esp_timer_get_time(void) { return 0; }
static void event(void) {}
'''
        code += production_functions((ROOT / 'src/services/solar_os_sessions.c').read_text(),
                                     ('solar_os_sessions_next_tick_wait_ms',))
        code += production_functions((ROOT / 'src/solar_os_jobs.c').read_text(),
                                     ('job_has_event', 'solar_os_jobs_next_tick_wait_ms'))
        code += r'''
int main(void) {
    assert(solar_os_sessions_next_tick_wait_ms(120) == 25);
    assert(solar_os_jobs_next_tick_wait_ms(120) == 25);
    solar_os_app_t app = {.event=event, .interval=16};
    session_state.sessions[0] = (solar_os_session_entry_t){.used=true, .app=&app,
        .tick_stats={.dispatch_count=1,.last_dispatch_ms=110}};
    solar_os_job_t job = {.event_with_user=event, .tick_interval_ms=25};
    job_entry_t entry = {.job=&job};
    job_runtimes[0] = (solar_os_job_runtime_t){.state=1,.entry=&entry,
        .tick_stats={.dispatch_count=1,.last_dispatch_ms=100}};
    /* The job is due at 125, before the app at 126, despite its longer period. */
    assert(solar_os_sessions_next_tick_wait_ms(120) == 6);
    assert(solar_os_jobs_next_tick_wait_ms(120) == 5);
    assert(solar_os_jobs_next_tick_wait_ms(130) == 0);
    job_runtimes[0].lifecycle_busy=true;
    session_state.sessions[0].suspended=true;
    assert(solar_os_sessions_next_tick_wait_ms(130) == 25);
    assert(solar_os_jobs_next_tick_wait_ms(130) == 25);
    job_runtimes[0].lifecycle_busy=false;
    job_runtimes[0].tick_stats.dispatch_count=0;
    assert(solar_os_jobs_next_tick_wait_ms(120) == 0);
    session_state.foreground_app=&app;
    assert(solar_os_sessions_next_tick_wait_ms(120) == 0);
    session_state.legacy_tick_app=&app;
    session_state.legacy_tick_stats=(solar_os_tick_stats_t){.dispatch_count=1,.last_dispatch_ms=115};
    assert(solar_os_sessions_next_tick_wait_ms(120) == 11);
    assert(!jobs_lock);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory, 'deadlines.c'); source.write_text(code)
            binary = Path(directory, 'deadlines')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I' + str(ROOT / 'src'),
                '-I' + str(ROOT / 'tests/host'), str(source),
                str(ROOT / 'src/solar_os_scheduler.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
