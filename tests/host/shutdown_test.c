#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/services/solar_os_shutdown.c"

static unsigned calls, power_calls, repeats;
static solar_os_shutdown_stage_t visited[16];
static esp_err_t step(solar_os_shutdown_stage_t current, void *user)
{
    (void)user;
    visited[calls++] = current;
    if (current == SOLAR_OS_SHUTDOWN_POWER) power_calls++;
    if (current == SOLAR_OS_SHUTDOWN_PORTS && repeats++ == 0) return ESP_ERR_NOT_FINISHED;
    return ESP_OK;
}
static esp_err_t failure_step(solar_os_shutdown_stage_t current, void *user)
{
    (void)user;
    if (current == SOLAR_OS_SHUTDOWN_APPS) solar_os_shutdown_note_failure(ESP_ERR_TIMEOUT);
    return step(current, NULL);
}
static esp_err_t pending_step(solar_os_shutdown_stage_t current, void *user)
{
    (void)current; (void)user;
    return ESP_ERR_NOT_FINISHED;
}
static void reset(void)
{
    atomic_store(&requested, 0);
    atomic_store(&failure, ESP_OK);
    started = false;
    stage = SOLAR_OS_SHUTDOWN_WAKE;
    calls = repeats = power_calls = 0;
}
int main(void)
{
    solar_os_shutdown_poll(step, NULL, 0);
    assert(calls == 0);
    assert(solar_os_shutdown_request() == ESP_OK);
    assert(solar_os_shutdown_request() == ESP_ERR_INVALID_STATE);
    for (unsigned i = 0; i < 20; i++) solar_os_shutdown_poll(step, NULL, i * 20);
    assert(solar_os_shutdown_stage() == SOLAR_OS_SHUTDOWN_COMPLETE);
    assert(solar_os_shutdown_requested() && power_calls == 1);
    assert(visited[0] == SOLAR_OS_SHUTDOWN_WAKE);
    assert(visited[1] == SOLAR_OS_SHUTDOWN_PORTS && visited[2] == SOLAR_OS_SHUTDOWN_PORTS);
    for (unsigned i = 3; i < calls; i++) assert(visited[i] == visited[i-1] + 1);
    assert(!strcmp(solar_os_shutdown_stage_name(SOLAR_OS_SHUTDOWN_STORAGE), "storage"));
    assert(!strcmp(solar_os_shutdown_stage_name(-1), "unknown"));
    reset();
    assert(solar_os_shutdown_request() == ESP_OK);
    for (unsigned i = 0; i < 20; i++) solar_os_shutdown_poll(failure_step, NULL, i * 20);
    assert(solar_os_shutdown_error() == ESP_ERR_TIMEOUT && power_calls == 0);
    assert(solar_os_shutdown_stage() == SOLAR_OS_SHUTDOWN_APPS);
    assert(solar_os_shutdown_requested());
    reset();
    assert(solar_os_shutdown_request() == ESP_OK);
    solar_os_shutdown_poll(pending_step, NULL, UINT32_MAX - 5000U);
    solar_os_shutdown_poll(pending_step, NULL, 4998U);
    assert(solar_os_shutdown_error() == ESP_OK);
    solar_os_shutdown_poll(pending_step, NULL, 4999U);
    assert(solar_os_shutdown_error() == ESP_ERR_TIMEOUT && power_calls == 0);
    reset();
    solar_os_shutdown_note_failure(ESP_FAIL);
    assert(solar_os_shutdown_error() == ESP_OK);
    assert(solar_os_shutdown_request() == ESP_OK);
    solar_os_shutdown_poll(NULL, NULL, 0);
    assert(solar_os_shutdown_error() == ESP_ERR_INVALID_ARG);
    puts("Shutdown: ordered cleanup, asynchronous barriers, failures and timeout wraparound OK");
    return 0;
}
