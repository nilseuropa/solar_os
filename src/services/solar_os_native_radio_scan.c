#include "solar_os_native_radio_scan.h"
#include "solar_os_config.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_RADIO_SCAN
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_radio_scan_abi.h"
#include "solar_os_native_support.h"
#include "solar_os_radio_scan.h"

#define SCAN_KIND SOLAR_OS_NATIVE_HANDLE_RADIO_SCAN

typedef struct {
    solar_os_radio_scan_worker_t *worker;
} scan_session_t;

static int scan_destroy(void *opaque)
{
    scan_session_t *session = opaque;
    const esp_err_t error = solar_os_radio_scan_worker_close(&session->worker, NULL);
    if (session->worker) return solar_os_native_result(error);
    solar_os_memory_free(session);
    return SOLAR_OS_NATIVE_OK;
}

static int scan_open(const char *name, const solar_os_radio_scan_config_t *config, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!name || !*name || !config) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    scan_session_t *session = solar_os_memory_calloc(1, sizeof(*session),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "radio.scan.session");
    if (!session) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    const solar_os_app_t *owner = solar_os_application_descriptor();
    solar_os_native_handle_t id;
    int result = owner ? solar_os_native_handle_create_owned(SCAN_KIND, session, scan_destroy, owner, &id) :
        solar_os_native_handle_create(SCAN_KIND, session, scan_destroy, &id);
    if (result) {
        solar_os_memory_free(session);
        return result;
    }
    result = solar_os_native_result(solar_os_radio_scan_worker_open(&session->worker, name, config));
    if (session->worker) *out = id;
    else (void)solar_os_native_handle_close_kind(id, SCAN_KIND);
    return result;
}

static void scan_copy(const solar_os_radio_scan_t *scan, void *opaque)
{
    solar_os_native_radio_scan_snapshot_v1_t *out = opaque;
    out->config = scan->config;
    out->receiver = scan->receiver;
    memcpy(out->name, scan->name, sizeof(out->name));
    memcpy(out->owner, scan->owner, sizeof(out->owner));
    out->running = scan->running;
    out->generation = scan->generation;
    out->sweeps = scan->sweeps;
    out->last_sweep_ms = scan->last_sweep_ms;
    out->bins = (uint16_t)scan->bins;
    out->bin = (uint16_t)scan->bin;
    out->rows = (uint16_t)scan->rows;
    out->history_count = (uint16_t)scan->history_count;
    out->history_head = (uint16_t)scan->history_head;
    out->recent_peak_frequency_hz = scan->recent_peak_frequency_hz;
    out->latest = scan->latest;
    out->recent_peak = scan->recent_peak;
    out->floor = scan->floor;
    out->peak = scan->peak;
    memcpy(out->levels, scan->levels, sizeof(out->levels));
    memcpy(out->peaks, scan->peaks, sizeof(out->peaks));
    memcpy(out->waterfall, scan->waterfall, sizeof(out->waterfall));
    memcpy(out->history, scan->history, sizeof(out->history));
    memcpy(out->history_ms, scan->history_ms, sizeof(out->history_ms));
    out->error = solar_os_native_result(scan->error);
}

static int scan_read(solar_os_native_handle_t id, solar_os_native_radio_scan_snapshot_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    void *state;
    int result = solar_os_native_handle_acquire_exclusive(id, SCAN_KIND, &state);
    if (result) return result;
    scan_session_t *session = state;
    result = session->worker ?
        solar_os_native_result(solar_os_radio_scan_worker_snapshot(session->worker, scan_copy, out)) :
        SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_native_handle_release_exclusive(id);
    return result;
}

static int scan_clear(solar_os_native_handle_t id)
{
    void *state;
    int result = solar_os_native_handle_acquire_exclusive(id, SCAN_KIND, &state);
    if (result) return result;
    scan_session_t *session = state;
    result = session->worker ? solar_os_native_result(solar_os_radio_scan_worker_clear(session->worker)) :
        SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_native_handle_release_exclusive(id);
    return result;
}

static int scan_done(solar_os_native_handle_t id, bool *done)
{
    if (!done) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *done = false;
    void *state;
    const int result = solar_os_native_handle_acquire_exclusive(id, SCAN_KIND, &state);
    if (result) return result;
    scan_session_t *session = state;
    *done = solar_os_radio_scan_worker_done(session->worker);
    solar_os_native_handle_release_exclusive(id);
    return SOLAR_OS_NATIVE_OK;
}

static int scan_stop(solar_os_native_handle_t id, solar_os_native_radio_scan_snapshot_v1_t *out)
{
    void *state;
    int result = solar_os_native_handle_acquire_exclusive(id, SCAN_KIND, &state);
    if (result) return result;
    scan_session_t *session = state;
    result = solar_os_native_result(solar_os_radio_scan_worker_close_snapshot(
        &session->worker, out ? scan_copy : NULL, out));
    solar_os_native_handle_release_exclusive(id);
    return result;
}

static int scan_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, SCAN_KIND); }

static const solar_os_native_radio_scan_api_v1_t radio_scan_api = {
    .abi_version = SOLAR_OS_NATIVE_RADIO_SCAN_ABI, .struct_size = sizeof(radio_scan_api),
    .open = scan_open, .read = scan_read, .clear = scan_clear, .done = scan_done,
    .stop = scan_stop, .close = scan_close,
};
#endif

const void *solar_os_native_radio_scan_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_radio_scan_dispatch.inc"
    (void)name; (void)abi; (void)minimum;
    return NULL;
}
