#include "solar_os_sump_job.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_logic.h"
#include "solar_os_port.h"

#define SUMP_JOB_TASK_STACK 5120
#define SUMP_JOB_TASK_PRIORITY (tskIDLE_PRIORITY + 3)
#define SUMP_JOB_PORT "cdc0"
#define SUMP_JOB_READ_SIZE 64U
#define SUMP_JOB_UPLOAD_SIZE 256U
#define SUMP_CLOCK_HZ 100000000UL

#define SUMP_RESET 0x00U
#define SUMP_RUN 0x01U
#define SUMP_ID 0x02U
#define SUMP_METADATA 0x04U
#define SUMP_XON 0x11U
#define SUMP_XOFF 0x13U
#define SUMP_SET_DIVIDER 0x80U
#define SUMP_SET_READ_DELAY 0x81U
#define SUMP_SET_FLAGS 0x82U

static const char *TAG = "solar_os_sump";

typedef struct {
    bool running;
    volatile bool stop_requested;
    TaskHandle_t task;
    solar_os_port_handle_t port;
    solar_os_logic_config_t config;
    uint8_t pending_command;
    uint8_t pending[4];
    uint8_t pending_count;
    uint32_t captures;
    uint64_t uploaded_bytes;
    uint32_t generation;
    esp_err_t last_error;
} sump_job_state_t;

static sump_job_state_t sump_job = {
    .port = SOLAR_OS_PORT_HANDLE_INIT,
    .last_error = ESP_OK,
};

static uint32_t sump_clamp_u32(uint32_t value, uint32_t min, uint32_t max)
{
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static uint32_t sump_le32(const uint8_t bytes[4])
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

static esp_err_t sump_write_all(const uint8_t *data, size_t len)
{
    size_t offset = 0;
    while (!sump_job.stop_requested && offset < len) {
        size_t written = 0;
        const esp_err_t err = solar_os_port_write(&sump_job.port,
                                                  &data[offset],
                                                  len - offset,
                                                  &written);
        if (err != ESP_OK) {
            return err;
        }
        if (written == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        offset += written;
    }
    return offset == len ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t sump_write_be32(uint8_t key, uint32_t value)
{
    const uint8_t bytes[] = {
        key,
        (uint8_t)(value >> 24),
        (uint8_t)(value >> 16),
        (uint8_t)(value >> 8),
        (uint8_t)value,
    };
    return sump_write_all(bytes, sizeof(bytes));
}

static esp_err_t sump_write_string(uint8_t key, const char *text)
{
    esp_err_t err = sump_write_all(&key, 1);
    if (err == ESP_OK) {
        err = sump_write_all((const uint8_t *)text, strlen(text) + 1U);
    }
    return err;
}

static esp_err_t sump_write_metadata(void)
{
    esp_err_t err = sump_write_string(0x01U, "SolarOS SUMP");
    if (err == ESP_OK) {
        err = sump_write_string(0x02U, "GPIO polling");
    }
    if (err == ESP_OK) {
        err = sump_write_be32(0x20U, sump_job.config.channel_count);
    }
    if (err == ESP_OK) {
        err = sump_write_be32(0x21U, SOLAR_OS_LOGIC_MAX_SAMPLES);
    }
    if (err == ESP_OK) {
        err = sump_write_be32(0x23U, SOLAR_OS_LOGIC_MAX_RATE_HZ);
    }
    if (err == ESP_OK) {
        err = sump_write_be32(0x24U, 2U);
    }
    if (err == ESP_OK) {
        const uint8_t end = 0;
        err = sump_write_all(&end, 1);
    }
    return err;
}

static esp_err_t sump_upload_capture(void)
{
    solar_os_logic_status_t status;
    esp_err_t err = solar_os_logic_get_status(&status);
    if (err != ESP_OK || !status.has_capture) {
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }

    uint8_t buffer[SUMP_JOB_UPLOAD_SIZE];
    size_t remaining = status.config.sample_count;
    while (!sump_job.stop_requested && remaining > 0) {
        const size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        const size_t start = remaining - chunk;
        size_t copied = 0;
        err = solar_os_logic_copy_samples(start, buffer, chunk, &copied);
        if (err != ESP_OK || copied != chunk) {
            return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
        }

        for (size_t left = 0, right = chunk - 1U; left < right; left++, right--) {
            const uint8_t value = buffer[left];
            buffer[left] = buffer[right];
            buffer[right] = value;
        }
        err = sump_write_all(buffer, chunk);
        if (err != ESP_OK) {
            return err;
        }
        remaining -= chunk;
        sump_job.uploaded_bytes += chunk;
    }
    return remaining == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t sump_run_capture(void)
{
    const esp_err_t err = solar_os_logic_capture(&sump_job.config);
    if (err != ESP_OK) {
        return err;
    }
    sump_job.captures++;
    return sump_upload_capture();
}

static void sump_handle_long_command(uint8_t command, const uint8_t data[4])
{
    const uint32_t value = sump_le32(data);
    if (command == SUMP_SET_DIVIDER) {
        const uint32_t rate = value == UINT32_MAX ? 1U : SUMP_CLOCK_HZ / (value + 1U);
        sump_job.config.sample_rate_hz = sump_clamp_u32(rate,
                                                       SOLAR_OS_LOGIC_MIN_RATE_HZ,
                                                       SOLAR_OS_LOGIC_MAX_RATE_HZ);
    } else if (command == SUMP_SET_READ_DELAY) {
        const uint32_t count = ((value & 0xffffU) + 1U) * 4U;
        sump_job.config.sample_count = sump_clamp_u32(count, 1U, SOLAR_OS_LOGIC_MAX_SAMPLES);
    }
}

static bool sump_command_has_payload(uint8_t command)
{
    return command == SUMP_SET_DIVIDER ||
        command == SUMP_SET_READ_DELAY ||
        command == SUMP_SET_FLAGS ||
        (command >= 0xc0U && command <= 0xcfU);
}

static esp_err_t sump_handle_byte(uint8_t byte)
{
    if (sump_job.pending_count > 0) {
        sump_job.pending[sump_job.pending_count - 1U] = byte;
        sump_job.pending_count++;
        if (sump_job.pending_count == 5U) {
            sump_handle_long_command(sump_job.pending_command, sump_job.pending);
            sump_job.pending_count = 0;
        }
        return ESP_OK;
    }

    if (sump_command_has_payload(byte)) {
        sump_job.pending_command = byte;
        sump_job.pending_count = 1;
        return ESP_OK;
    }

    if (byte == SUMP_RESET) {
        sump_job.pending_count = 0;
        return ESP_OK;
    }
    if (byte == SUMP_RUN) {
        return sump_run_capture();
    }
    if (byte == SUMP_ID) {
        return sump_write_all((const uint8_t *)"1ALS", 4);
    }
    if (byte == SUMP_METADATA) {
        return sump_write_metadata();
    }
    if (byte == SUMP_XON || byte == SUMP_XOFF) {
        return ESP_OK;
    }
    return ESP_OK;
}

static void sump_cleanup(void)
{
    if (solar_os_port_handle_valid(&sump_job.port)) {
        (void)solar_os_port_release(&sump_job.port);
    }
    sump_job.running = false;
    sump_job.stop_requested = false;
    sump_job.task = NULL;
    sump_job.pending_count = 0;
}

static void sump_task(void *arg)
{
    (void)arg;
    uint8_t buffer[SUMP_JOB_READ_SIZE];
    esp_err_t fatal_error = ESP_OK;

    SOLAR_OS_LOGI(TAG,
                  "started: port=%s channels=%u",
                  SUMP_JOB_PORT,
                  (unsigned)sump_job.config.channel_count);

    while (!sump_job.stop_requested) {
        size_t read_len = 0;
        const esp_err_t err = solar_os_port_read(&sump_job.port,
                                                 buffer,
                                                 sizeof(buffer),
                                                 20U,
                                                 &read_len);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            fatal_error = err;
            break;
        }
        for (size_t i = 0; i < read_len && !sump_job.stop_requested; i++) {
            const esp_err_t command_err = sump_handle_byte(buffer[i]);
            if (command_err != ESP_OK && command_err != ESP_ERR_INVALID_STATE) {
                sump_job.last_error = command_err;
            }
        }
        if (read_len == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    if (fatal_error != ESP_OK) {
        sump_job.last_error = fatal_error;
        (void)solar_os_jobs_mark_stopped(solar_os_sump_job.name,
                                         sump_job.generation,
                                         fatal_error);
    }
    SOLAR_OS_LOGI(TAG,
                  "stopped: captures=%lu uploaded=%llu error=%s",
                  (unsigned long)sump_job.captures,
                  (unsigned long long)sump_job.uploaded_bytes,
                  esp_err_to_name(sump_job.last_error));
    sump_cleanup();
    vTaskDelete(NULL);
}

static bool sump_parse_pin(const char *text, uint8_t *pin)
{
    if (text == NULL || text[0] == '\0' || pin == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value > UINT8_MAX) {
        return false;
    }
    *pin = (uint8_t)value;
    return true;
}

static esp_err_t sump_add_pin(solar_os_logic_config_t *config, uint8_t pin)
{
    if (config->channel_count >= SOLAR_OS_LOGIC_MAX_CHANNELS) {
        return ESP_ERR_INVALID_SIZE;
    }
    config->pins[config->channel_count++] = pin;
    return ESP_OK;
}

static esp_err_t sump_parse_pins(int argc, char **argv, solar_os_logic_config_t *config)
{
    int first_arg = 0;
    if (argc > 0 && argv != NULL && argv[0] != NULL &&
        strcmp(argv[0], solar_os_sump_job.name) == 0) {
        first_arg = 1;
    }
    if (argc <= first_arg) {
        return solar_os_logic_default_config(config);
    }
    if (argv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));
    config->sample_rate_hz = SOLAR_OS_LOGIC_DEFAULT_RATE_HZ;
    config->sample_count = SOLAR_OS_LOGIC_DEFAULT_SAMPLES;

    for (int i = first_arg; i < argc; i++) {
        if (argv[i] == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        char arg[SOLAR_OS_APP_ARG_LEN];
        strlcpy(arg, argv[i], sizeof(arg));
        char *save = NULL;
        for (char *token = strtok_r(arg, ",", &save);
             token != NULL;
             token = strtok_r(NULL, ",", &save)) {
            uint8_t pin = 0;
            if (!sump_parse_pin(token, &pin)) {
                return ESP_ERR_INVALID_ARG;
            }
            const esp_err_t err = sump_add_pin(config, pin);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return solar_os_logic_validate_config(config);
}

static esp_err_t sump_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (sump_job.running || sump_job.task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_logic_config_t config;
    esp_err_t err = sump_parse_pins(argc, argv, &config);
    if (err != ESP_OK) {
        return err;
    }
    err = solar_os_jobs_get_generation(solar_os_sump_job.name, &sump_job.generation);
    if (err != ESP_OK) {
        return err;
    }

    solar_os_port_info_t info;
    err = solar_os_port_get_info(SUMP_JOB_PORT, &info);
    if (err != ESP_OK) {
        return err;
    }
    if ((info.capabilities & (SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE)) !=
        (SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = solar_os_jobs_claim_port(solar_os_sump_job.name, SUMP_JOB_PORT, &sump_job.port);
    if (err != ESP_OK) {
        return err;
    }

    sump_job.config = config;
    sump_job.running = true;
    sump_job.stop_requested = false;
    sump_job.pending_count = 0;
    sump_job.captures = 0;
    sump_job.uploaded_bytes = 0;
    sump_job.last_error = ESP_OK;

    char pins[48] = "";
    for (uint8_t i = 0; i < config.channel_count; i++) {
        char item[8];
        snprintf(item, sizeof(item), "%s%u", i == 0 ? "" : ",", (unsigned)config.pins[i]);
        strlcat(pins, item, sizeof(pins));
    }
    (void)solar_os_jobs_note_resource(solar_os_sump_job.name,
                                      SOLAR_OS_JOB_RESOURCE_CUSTOM,
                                      "logic-gpio",
                                      pins);

    if (xTaskCreate(sump_task,
                    "sump_job",
                    SUMP_JOB_TASK_STACK,
                    NULL,
                    SUMP_JOB_TASK_PRIORITY,
                    &sump_job.task) != pdPASS) {
        sump_cleanup();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void sump_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (!sump_job.running && sump_job.task == NULL) {
        return;
    }

    sump_job.stop_requested = true;
    if (sump_job.task != NULL && sump_job.task != xTaskGetCurrentTaskHandle()) {
        for (uint32_t i = 0; i < 200U && sump_job.task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }
}

const solar_os_job_t solar_os_sump_job = {
    .name = "sump",
    .summary = "SUMP logic analyzer on cdc0",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = sump_start,
    .stop = sump_stop,
    .event = NULL,
};
