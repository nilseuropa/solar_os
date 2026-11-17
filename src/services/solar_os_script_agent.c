#include "solar_os_script_agent.h"

#include <string.h>
#include "solar_os_agent_images.h"
#include "solar_os_media_sample_source.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

typedef struct {
    const solar_os_script_agent_request_t *request;
    solar_os_script_agent_result_t *result;
} script_agent_capture_t;

static bool script_agent_cancelled(void *user_data)
{
    script_agent_capture_t *capture = user_data;
    return capture->request->should_cancel != NULL &&
        capture->request->should_cancel(capture->request->user_data);
}

static esp_err_t script_agent_event(const solar_os_agent_event_t *event, void *user_data)
{
    script_agent_capture_t *capture = user_data;
    solar_os_script_agent_result_t *result = capture->result;
    if (script_agent_cancelled(capture)) return ESP_ERR_INVALID_STATE;
    if (event->type == SOLAR_OS_AGENT_EVENT_TEXT_DELTA) {
        const size_t length = strlen(event->text);
        const size_t remaining = SOLAR_OS_SCRIPT_AGENT_TEXT_MAX - result->text_len;
        const size_t copy = length < remaining ? length : remaining;
        memcpy(result->text + result->text_len, event->text, copy);
        result->text_len += copy;
        result->text[result->text_len] = '\0';
        result->truncated |= copy < length;
    } else if (event->type == SOLAR_OS_AGENT_EVENT_USAGE) {
        /* Match the foreground app: report the latest provider turn's usage. */
        result->prompt_tokens = event->prompt_tokens;
        result->completion_tokens = event->completion_tokens;
        result->total_tokens = event->total_tokens;
    }
    return ESP_OK;
}

void solar_os_script_agent_result_clear(solar_os_script_agent_result_t *result)
{
    if (result == NULL) return;
    solar_os_memory_free(result->text);
    memset(result, 0, sizeof(*result));
}

esp_err_t solar_os_script_agent_ask(const solar_os_script_agent_request_t *request,
    solar_os_script_agent_result_t *result)
{
    if (result == NULL) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    if (request == NULL || request->prompt == NULL || !request->prompt[0] ||
        strlen(request->prompt) >= SOLAR_OS_AGENT_PROMPT_MAX ||
        request->image_count > SOLAR_OS_AGENT_IMAGES_MAX ||
        (request->conversation_id != NULL &&
         strlen(request->conversation_id) >= SOLAR_OS_AGENT_CONVERSATION_ID_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    script_agent_capture_t capture = {.request = request, .result = result};
    if (script_agent_cancelled(&capture)) return ESP_ERR_INVALID_STATE;
    result->text = solar_os_memory_calloc(1, SOLAR_OS_SCRIPT_AGENT_TEXT_MAX + 1U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "script.agent.text");
    if (result->text == NULL) return ESP_ERR_NO_MEM;

    solar_os_media_sample_t *images[SOLAR_OS_AGENT_IMAGES_MAX] = {0};
    size_t remaining = SOLAR_OS_AGENT_IMAGE_BYTES_MAX;
    esp_err_t error = ESP_OK;
    for (size_t i = 0; i < request->image_count && error == ESP_OK; i++) {
        const char *source = request->images[i];
        char path[SOLAR_OS_STORAGE_PATH_MAX];
        if (source == NULL || !source[0] ||
            strlen(source) >= SOLAR_OS_MEDIA_SAMPLE_SOURCE_MAX) {
            error = ESP_ERR_INVALID_ARG;
            break;
        }
        if (strncmp(source, "stream:", 7) && strncmp(source, "rtsp://", 7) &&
            strncmp(source, "pipeline:", 9)) {
            error = solar_os_storage_resolve_path(source, path, sizeof(path));
            source = path;
        }
        const solar_os_media_sample_source_options_t options = {
            .owner = "script:agent", .max_bytes = remaining, .timeout_ms = 5000,
            .cancel = script_agent_cancelled, .user = &capture,
        };
        if (error == ESP_OK) error = remaining ?
            solar_os_media_sample_source_read(source, &options, &images[i]) :
            ESP_ERR_INVALID_SIZE;
        if (error == ESP_OK) error = solar_os_agent_images_validate(images, i + 1U);
        if (error == ESP_OK) remaining -= solar_os_media_sample_image(images[i])->length;
    }
    const solar_os_agent_request_t native = {
        .prompt = request->prompt, .images = images, .image_count = request->image_count,
        .conversation_id = request->conversation_id,
        .next_conversation_id = request->conversation_id != NULL ? result->conversation_id : NULL,
        .next_conversation_id_len = sizeof(result->conversation_id),
        .event_handler = script_agent_event, .should_cancel = script_agent_cancelled,
        .user_data = &capture,
    };
    if (error == ESP_OK) error = script_agent_cancelled(&capture) ?
        ESP_ERR_INVALID_STATE : solar_os_agent_run(&native);
    for (size_t i = 0; i < SOLAR_OS_AGENT_IMAGES_MAX; i++) solar_os_media_sample_release(images[i]);
    if (error != ESP_OK) solar_os_script_agent_result_clear(result);
    return error;
}
