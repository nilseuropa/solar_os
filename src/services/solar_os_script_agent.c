#include "solar_os_script_agent.h"

#include <string.h>
#include "solar_os_agent_images.h"
#include "solar_os_media_sample_source.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

typedef struct {
    const solar_os_agent_request_t *request;
    solar_os_native_agent_answer_v1_t *answer;
    char *text;
    size_t capacity;
} agent_answer_capture_t;

static bool script_agent_cancelled(void *user)
{
    const solar_os_script_agent_request_t *request = user;
    return request->should_cancel && request->should_cancel(request->user_data);
}

static bool agent_answer_cancelled(void *user)
{
    const agent_answer_capture_t *capture = user;
    return capture->request->should_cancel &&
        capture->request->should_cancel(capture->request->user_data);
}

static esp_err_t agent_answer_event(const solar_os_agent_event_t *event, void *user)
{
    agent_answer_capture_t *capture = user;
    solar_os_native_agent_answer_v1_t *answer = capture->answer;
    if (agent_answer_cancelled(capture)) return ESP_ERR_INVALID_STATE;
    if (event->type == SOLAR_OS_AGENT_EVENT_TEXT_DELTA) {
        const size_t length = strlen(event->text);
        const size_t remaining = capture->capacity - 1U - answer->text_len;
        const size_t copy = length < remaining ? length : remaining;
        memcpy(capture->text + answer->text_len, event->text, copy);
        answer->text_len += copy;
        capture->text[answer->text_len] = '\0';
        answer->truncated |= copy < length;
    } else if (event->type == SOLAR_OS_AGENT_EVENT_USAGE) {
        answer->prompt_tokens = event->prompt_tokens;
        answer->completion_tokens = event->completion_tokens;
        answer->total_tokens = event->total_tokens;
    }
    return ESP_OK;
}

esp_err_t solar_os_script_agent_capture_answer(const solar_os_agent_request_t *request,
    char *text, size_t capacity, solar_os_native_agent_answer_v1_t *answer)
{
    _Static_assert(sizeof(answer->conversation_id) == SOLAR_OS_AGENT_CONVERSATION_ID_MAX,
                   "Agent answer conversation ID capacity");
    if (!request || !text || !capacity || !answer) return ESP_ERR_INVALID_ARG;
    char selected_id[SOLAR_OS_AGENT_CONVERSATION_ID_MAX];
    const bool have_id = request->conversation_id != NULL;
    const size_t id_len = have_id ? strlcpy(selected_id, request->conversation_id, sizeof(selected_id)) : 0;
    *answer = (solar_os_native_agent_answer_v1_t){0};
    text[0] = '\0';
    if (id_len >= sizeof(selected_id)) return ESP_ERR_INVALID_ARG;
    agent_answer_capture_t capture = {.request = request, .answer = answer,
                                      .text = text, .capacity = capacity};
    const solar_os_agent_request_t core = {
        .prompt = request->prompt,
        .images = request->images,
        .image_count = request->image_count,
        .audio = request->audio,
        .conversation_id = have_id ? selected_id : NULL,
        .storage_cwd = request->storage_cwd,
        .next_conversation_id = request->conversation_id ? answer->conversation_id : NULL,
        .next_conversation_id_len = sizeof(answer->conversation_id),
        .event_handler = agent_answer_event,
        .should_cancel = agent_answer_cancelled,
        .user_data = &capture,
    };
    const esp_err_t err = agent_answer_cancelled(&capture) ?
        ESP_ERR_INVALID_STATE : solar_os_agent_run(&core);
    if (err != ESP_OK) {
        *answer = (solar_os_native_agent_answer_v1_t){0};
        text[0] = '\0';
    }
    return err;
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
    if (script_agent_cancelled((void *)request)) return ESP_ERR_INVALID_STATE;
    result->text = solar_os_memory_calloc(1, SOLAR_OS_SCRIPT_AGENT_TEXT_MAX + 1U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "script.agent.text");
    if (result->text == NULL) return ESP_ERR_NO_MEM;

    solar_os_media_sample_t *images[SOLAR_OS_AGENT_IMAGES_MAX] = {0};
    solar_os_media_sample_t *audio = NULL;
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
            .cancel = script_agent_cancelled, .user = (void *)request,
        };
        if (error == ESP_OK) error = remaining ?
            solar_os_media_sample_source_read(source, &options, &images[i]) :
            ESP_ERR_INVALID_SIZE;
        if (error == ESP_OK) error = solar_os_agent_images_validate(images, i + 1U);
        if (error == ESP_OK) remaining -= solar_os_media_sample_image(images[i])->length;
    }
    if (error == ESP_OK && request->audio_file != NULL) {
        char path[SOLAR_OS_STORAGE_PATH_MAX];
        error = solar_os_storage_resolve_path(request->audio_file, path, sizeof(path));
        if (error == ESP_OK) error = solar_os_media_sample_open_audio_bounded(path,
            SOLAR_OS_AGENT_AUDIO_BYTES_MAX, script_agent_cancelled, (void *)request, &audio);
    }
    const solar_os_agent_request_t native = {
        .prompt = request->prompt, .images = images, .image_count = request->image_count,
        .audio = audio,
        .conversation_id = request->conversation_id,
        .should_cancel = script_agent_cancelled,
        .user_data = (void *)request,
    };
    if (error == ESP_OK) {
        solar_os_native_agent_answer_v1_t answer;
        error = solar_os_script_agent_capture_answer(&native, result->text,
            SOLAR_OS_SCRIPT_AGENT_TEXT_MAX + 1U, &answer);
        if (error == ESP_OK) {
            result->text_len = answer.text_len;
            result->truncated = answer.truncated;
            memcpy(result->conversation_id, answer.conversation_id, sizeof(result->conversation_id));
            result->prompt_tokens = answer.prompt_tokens;
            result->completion_tokens = answer.completion_tokens;
            result->total_tokens = answer.total_tokens;
        }
    }
    for (size_t i = 0; i < SOLAR_OS_AGENT_IMAGES_MAX; i++) solar_os_media_sample_release(images[i]);
    solar_os_media_sample_release(audio);
    if (error != ESP_OK) solar_os_script_agent_result_clear(result);
    return error;
}
