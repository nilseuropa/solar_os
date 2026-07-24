#include "solar_os_bridge_job.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_port.h"
#include "solar_os_task.h"

#define BRIDGE_JOB_TASK_STACK 4096
#define BRIDGE_JOB_TASK_PRIORITY (tskIDLE_PRIORITY + 3)
#define BRIDGE_JOB_BUFFER_SIZE 512
#define BRIDGE_JOB_READ_TIMEOUT_MS 10U

static const char *TAG = "solar_os_bridge";

typedef struct {
    bool running;
    volatile bool stop_requested;
    TaskHandle_t task;
    solar_os_port_handle_t port_a;
    solar_os_port_handle_t port_b;
    char port_a_name[SOLAR_OS_PORT_NAME_MAX];
    char port_b_name[SOLAR_OS_PORT_NAME_MAX];
    uint32_t bytes_a_to_b;
    uint32_t bytes_b_to_a;
    uint32_t read_failures;
    uint32_t write_failures;
    esp_err_t last_error;
} bridge_job_state_t;

static bridge_job_state_t bridge_job = {
    .port_a = SOLAR_OS_PORT_HANDLE_INIT,
    .port_b = SOLAR_OS_PORT_HANDLE_INIT,
    .last_error = ESP_OK,
};

static esp_err_t bridge_job_validate_port(const char *name)
{
    solar_os_port_info_t info;

    const esp_err_t err = solar_os_port_get_info(name, &info);
    if (err != ESP_OK) {
        return err;
    }
    if (info.claimed) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((info.capabilities & (SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE)) !=
        (SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static void bridge_job_cleanup(void)
{
    if (solar_os_port_handle_valid(&bridge_job.port_a)) {
        (void)solar_os_port_release(&bridge_job.port_a);
    }
    if (solar_os_port_handle_valid(&bridge_job.port_b)) {
        (void)solar_os_port_release(&bridge_job.port_b);
    }

    bridge_job.running = false;
    bridge_job.stop_requested = false;
    bridge_job.task = NULL;
    bridge_job.port_a_name[0] = '\0';
    bridge_job.port_b_name[0] = '\0';
}

static esp_err_t bridge_job_write_all(const solar_os_port_handle_t *dst,
                                      const uint8_t *data,
                                      size_t len)
{
    size_t offset = 0;
    while (!bridge_job.stop_requested && offset < len) {
        size_t written = 0;
        const esp_err_t err = solar_os_port_write(dst, &data[offset], len - offset, &written);
        if (written > 0) {
            offset += written;
        }
        if (err != ESP_OK) {
            bridge_job.write_failures++;
            bridge_job.last_error = err;
            return err;
        }
        if (written == 0) {
            bridge_job.write_failures++;
            bridge_job.last_error = ESP_FAIL;
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static bool bridge_job_forward_once(const solar_os_port_handle_t *src,
                                    const solar_os_port_handle_t *dst,
                                    uint32_t *byte_counter,
                                    uint8_t *buffer,
                                    size_t buffer_len)
{
    size_t read_len = 0;
    const esp_err_t err = solar_os_port_read(src,
                                             buffer,
                                             buffer_len,
                                             BRIDGE_JOB_READ_TIMEOUT_MS,
                                             &read_len);
    if (err != ESP_OK) {
        if (err != ESP_ERR_TIMEOUT) {
            bridge_job.read_failures++;
            bridge_job.last_error = err;
        }
        return false;
    }
    if (read_len == 0) {
        return false;
    }

    const esp_err_t write_err = bridge_job_write_all(dst, buffer, read_len);
    if (write_err == ESP_OK && byte_counter != NULL) {
        *byte_counter += (uint32_t)read_len;
    }
    return true;
}

static void bridge_job_task(void *arg)
{
    bridge_job_state_t *state = (bridge_job_state_t *)arg;
    uint8_t buffer[BRIDGE_JOB_BUFFER_SIZE];

    SOLAR_OS_LOGI(TAG,
                  "started: %s <-> %s",
                  state->port_a_name,
                  state->port_b_name);

    while (!state->stop_requested) {
        const bool moved_a =
            bridge_job_forward_once(&state->port_a,
                                    &state->port_b,
                                    &state->bytes_a_to_b,
                                    buffer,
                                    sizeof(buffer));
        const bool moved_b =
            bridge_job_forward_once(&state->port_b,
                                    &state->port_a,
                                    &state->bytes_b_to_a,
                                    buffer,
                                    sizeof(buffer));
        if (!moved_a && !moved_b) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    SOLAR_OS_LOGI(TAG,
                  "stopped: %s->%s=%" PRIu32 " %s->%s=%" PRIu32 " read_fail=%" PRIu32 " write_fail=%" PRIu32,
                  state->port_a_name,
                  state->port_b_name,
                  state->bytes_a_to_b,
                  state->port_b_name,
                  state->port_a_name,
                  state->bytes_b_to_a,
                  state->read_failures,
                  state->write_failures);

    bridge_job_cleanup();
    solar_os_task_delete(NULL);
}

static esp_err_t bridge_job_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;

    if (argc != 3 || argv == NULL ||
        argv[1] == NULL || argv[1][0] == '\0' ||
        argv[2] == NULL || argv[2][0] == '\0' ||
        strcmp(argv[1], argv[2]) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bridge_job.running || bridge_job.task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = bridge_job_validate_port(argv[1]);
    if (err != ESP_OK) {
        return err;
    }
    err = bridge_job_validate_port(argv[2]);
    if (err != ESP_OK) {
        return err;
    }

    err = solar_os_jobs_claim_port(solar_os_bridge_job.name, argv[1], &bridge_job.port_a);
    if (err != ESP_OK) {
        return err;
    }
    err = solar_os_jobs_claim_port(solar_os_bridge_job.name, argv[2], &bridge_job.port_b);
    if (err != ESP_OK) {
        (void)solar_os_port_release(&bridge_job.port_a);
        return err;
    }

    bridge_job.running = true;
    bridge_job.stop_requested = false;
    bridge_job.bytes_a_to_b = 0;
    bridge_job.bytes_b_to_a = 0;
    bridge_job.read_failures = 0;
    bridge_job.write_failures = 0;
    bridge_job.last_error = ESP_OK;
    strlcpy(bridge_job.port_a_name, argv[1], sizeof(bridge_job.port_a_name));
    strlcpy(bridge_job.port_b_name, argv[2], sizeof(bridge_job.port_b_name));

    if (solar_os_task_create_pinned(bridge_job_task,
                                    "bridge_job",
                                    BRIDGE_JOB_TASK_STACK,
                                    &bridge_job,
                                    BRIDGE_JOB_TASK_PRIORITY,
                                    &bridge_job.task,
                                    tskNO_AFFINITY,
                                    SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        bridge_job_cleanup();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void bridge_job_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    if (!bridge_job.running && bridge_job.task == NULL) {
        return;
    }

    bridge_job.stop_requested = true;
    if (bridge_job.task != NULL && bridge_job.task != xTaskGetCurrentTaskHandle()) {
        for (uint32_t i = 0; i < 80 && bridge_job.task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }
}

const solar_os_job_t solar_os_bridge_job = {
    .name = "bridge",
    .summary = "raw bidirectional port bridge",
    .start = bridge_job_start,
    .stop = bridge_job_stop,
    .event = NULL,
    .worker_stack_bytes = BRIDGE_JOB_TASK_STACK,
};
