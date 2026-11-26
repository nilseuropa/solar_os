from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeEnginesTest(unittest.TestCase):
    def test_shared_counters_and_reset_during_measured_work(self):
        fixtures = ROOT / 'tests/host'
        services = ROOT / 'src/services'
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / 'esp_timer.h').write_text('#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n')
            (stage / 'test.c').write_text(r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include "solar_os_engines.h"
#include "solar_os_native_diagnostics.h"

atomic_uint registry_mutex_initializations;
static int64_t now = 100;
static const solar_os_native_engines_api_v1_t *engines;
int64_t esp_timer_get_time(void) { return now; }

static int inner(void *user, uint64_t *units)
{
    solar_os_engine_stats_t stats;
    assert(user && engines->find("cpu", &stats) && stats.active_count == 2);
    now += 30;
    *units = 4;
    return SOLAR_OS_NATIVE_OK;
}

static int outer(void *user, uint64_t *units)
{
    now += 10;
    assert(!engines->measure("cpu", "probe", "inner", inner, user));
    now += 20;
    *units = 3;
    return SOLAR_OS_NATIVE_ERROR_CANCELLED;
}

static int reset_during_work(void *user, uint64_t *units)
{
    (void)user;
    now += 20;
    engines->reset();
    now += 10;
    *units = 1;
    return SOLAR_OS_NATIVE_OK;
}

static void *query(void *argument)
{
    pthread_barrier_t *barrier = argument;
    pthread_barrier_wait(barrier);
    assert(engines->count() == 1);
    return NULL;
}

int main(void)
{
    engines = solar_os_native_diagnostics_get_service("engines", 1, sizeof(*engines));
    assert(engines);
    pthread_barrier_t barrier;
    pthread_t tasks[8];
    assert(!pthread_barrier_init(&barrier, NULL, 8));
    for (size_t i = 0; i < 8; ++i) assert(!pthread_create(&tasks[i], NULL, query, &barrier));
    for (size_t i = 0; i < 8; ++i) assert(!pthread_join(tasks[i], NULL));
    assert(!pthread_barrier_destroy(&barrier) && atomic_load(&registry_mutex_initializations) == 1);
    solar_os_engine_stats_t stats;
    assert(engines->info(0, &stats) && !strcmp(stats.name, "cpu"));
    assert(!engines->info(1, &stats));
    assert(engines->measure("missing", "probe", NULL, inner, NULL) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(engines->measure("cpu", "probe", "outer", outer, &stats) == SOLAR_OS_NATIVE_ERROR_CANCELLED);
    assert(solar_os_engine_get_by_name("cpu", &stats));
    assert(!stats.active && stats.op_count == 2 && stats.unit_count == 7);
    assert(stats.work_us == 90 && stats.busy_us == 60 && stats.last_us == 60);
    assert(!engines->measure("cpu", "probe", "reset", reset_during_work, NULL));
    assert(engines->find("cpu", &stats) && !stats.active && !stats.op_count && stats.busy_us == 10);
    assert(!engines->record("cpu", "probe", "external", 21, 5));
    assert(engines->find("cpu", &stats) && stats.work_us == 21 && stats.busy_us == 31);
    assert(stats.max_us == 21 && stats.unit_count == 5 && !strcmp(stats.label, "external"));
    return 0;
}
''')
            subprocess.run([
                'cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-DSOLAR_OS_PACKAGE_SERVICE_ENGINES=1', '-include', str(fixtures / 'compat.h'),
                *['-I' + str(path) for path in (stage, fixtures / 'registry_stubs', fixtures, ROOT / 'include', ROOT / 'src', services)],
                str(stage / 'test.c'), str(services / 'solar_os_engines.c'),
                str(services / 'solar_os_native_diagnostics.c'), '-o', str(stage / 'test'),
            ], check=True)
            subprocess.run([str(stage / 'test')], check=True, timeout=10)
