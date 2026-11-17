/* Reuse the real provider, image serializers, and instrumented HTTP/allocator
 * doubles. Native agent dispatch below isolates script ownership and policy. */
#define SOLAR_OS_AGENT_IMAGES_TEST_SUPPORT_ONLY
#include "agent_images_test.c"
#include "solar_os_script_agent.h"
#include "solar_os_media_sample_source.h"
#include "solar_os_storage.h"
#include <unistd.h>

static bool script_cancel, cancel_during_source, cancel_during_run, long_answer;
static int source_reads, agent_runs;
static esp_err_t agent_error;
static FILE *body_output;
static const char *test_endpoint = "https://test/responses";
static void (*after_run)(void);
static solar_os_agent_status_t service_status;

static bool script_should_cancel(void *user) { (void)user; return script_cancel; }
esp_err_t solar_os_storage_resolve_path(const char *input, char *out, size_t capacity)
{
    int size = !strncmp(input, "/tmp/", 5) ? snprintf(out, capacity, "%s", input) :
        snprintf(out, capacity, "/resolved/%s", input[0] == '/' ? input + 1 : input);
    return size >= 0 && (size_t)size < capacity ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t solar_os_media_sample_source_read(const char *source,
    const solar_os_media_sample_source_options_t *options, solar_os_media_sample_t **out)
{
    source_reads++;
    *out = NULL;
    assert(!strcmp(options->owner, "script:agent") && options->timeout_ms == 5000);
    assert(options->cancel && !options->cancel(options->user));
    if (strstr(source, "missing")) return ESP_ERR_NOT_FOUND;
    if (strstr(source, "unsupported")) return ESP_ERR_NOT_SUPPORTED;
    const uint8_t jpeg[] = {255, 216, 255, 0, 1, 2, 3, 4};
    size_t length = strstr(source, "large") ? SOLAR_OS_AGENT_IMAGE_BYTES_MAX - 4 : sizeof(jpeg);
    if (length > options->max_bytes) return ESP_ERR_INVALID_SIZE;
    uint8_t *data = calloc(1, length);
    assert(data);
    memcpy(data, jpeg, sizeof(jpeg));
    const solar_os_media_sample_metadata_t metadata = {.source = source};
    esp_err_t error = solar_os_media_sample_copy_image(data, length, &metadata, out);
    free(data);
    if (cancel_during_source) script_cancel = true;
    return error;
}

esp_err_t solar_os_agent_run(const solar_os_agent_request_t *request)
{
    agent_runs++;
    assert(request->run_script == NULL && request->script_languages == 0 && request->confirm_tool == NULL);
    assert(request->should_cancel && request->event_handler);
    if (cancel_during_run) script_cancel = true;
    if (request->should_cancel(request->user_data)) return ESP_ERR_INVALID_STATE;
    if (agent_error) return agent_error;
    solar_os_agent_provider_config_t config = {0};
    strcpy(config.endpoint, test_endpoint);
    strcpy(config.model, "vision-test");
    solar_os_agent_provider_turn_t turn = {
        .prompt = request->prompt, .images = request->images, .image_count = request->image_count,
        .audio = request->audio,
    };
    if (body_output) record(body_output, &config, &turn);
    solar_os_agent_provider_result_t result;
    esp_err_t error = agent_openai_run_turn(&config, &turn, request->event_handler, request->user_data, &result);
    if (error) return error;
    solar_os_agent_event_t usage = {.type = SOLAR_OS_AGENT_EVENT_USAGE,
        .prompt_tokens = 10, .completion_tokens = 2, .total_tokens = 12};
    assert(!request->event_handler(&usage, request->user_data));
    usage.prompt_tokens = 30; usage.completion_tokens = 3; usage.total_tokens = 33;
    assert(!request->event_handler(&usage, request->user_data));
    if (long_answer) {
        solar_os_agent_event_t event = {.type = SOLAR_OS_AGENT_EVENT_TEXT_DELTA};
        memset(event.text, 'x', sizeof(event.text) - 1);
        for (int i = 0; i < 100; i++) assert(!request->event_handler(&event, request->user_data));
    }
    if (request->next_conversation_id) {
        assert(request->next_conversation_id_len >= 3);
        assert(request->conversation_id && (!request->conversation_id[0] || !strcmp(request->conversation_id, "c1")));
        strcpy(request->next_conversation_id, "c1");
    }
    if (after_run) after_run();
    return ESP_OK;
}

esp_err_t solar_os_agent_get_status(solar_os_agent_status_t *out) { *out = service_status; return ESP_OK; }
esp_err_t solar_os_agent_set_endpoint(const char *text) { strlcpy(service_status.endpoint, text, sizeof(service_status.endpoint)); return ESP_OK; }
esp_err_t solar_os_agent_set_model(const char *text) { strlcpy(service_status.model, text, sizeof(service_status.model)); return ESP_OK; }
esp_err_t solar_os_agent_set_api_key(const char *text) { service_status.api_key_set = *text != 0; return ESP_OK; }
esp_err_t solar_os_agent_set_reasoning_effort(const char *text) { strlcpy(service_status.reasoning_effort, text, sizeof(service_status.reasoning_effort)); return ESP_OK; }
const char *solar_os_agent_tool_policy_name(solar_os_agent_tool_policy_t policy)
{ return policy == SOLAR_OS_AGENT_TOOL_POLICY_OFF ? "off" : "confirm"; }
esp_err_t solar_os_agent_parse_tool_policy(const char *text, solar_os_agent_tool_policy_t *policy)
{ if (strcmp(text, "off")) return ESP_ERR_INVALID_ARG; *policy = SOLAR_OS_AGENT_TOOL_POLICY_OFF; return ESP_OK; }
esp_err_t solar_os_agent_set_tool_policy(solar_os_agent_tool_policy_t policy) { service_status.tool_policy = policy; return ESP_OK; }
esp_err_t solar_os_agent_set_max_tools(uint8_t count) { assert(count >= 1 && count <= 32); service_status.max_tools = count; return ESP_OK; }
esp_err_t solar_os_agent_cancel(void) { script_cancel = true; return ESP_OK; }
esp_err_t solar_os_agent_forget(void) { memset(&service_status, 0, sizeof(service_status)); return ESP_OK; }
esp_err_t solar_os_agent_conversation_get(const char *id, solar_os_agent_conversation_info_t *info)
{
    if (strcmp(id, "c1")) return ESP_ERR_NOT_FOUND;
    *info = (solar_os_agent_conversation_info_t){.turn_count = 2, .created_at = 5000000000ULL};
    strcpy(info->id, "c1"); strcpy(info->title, "Image question");
    return ESP_OK;
}
esp_err_t solar_os_agent_conversations_list(solar_os_agent_conversation_info_t *items, size_t size, size_t *count)
{ assert(size >= 1); *count = 1; return solar_os_agent_conversation_get("c1", items); }
esp_err_t solar_os_agent_conversation_delete(const char *id) { return strcmp(id, "c1") ? ESP_ERR_NOT_FOUND : ESP_OK; }

/* A valid tiny mono PCM WAV, and an ID3-tagged MP3 header for container tests.
 * The HTTP double checks transport and text handling; it does not decode audio. */
const uint8_t script_agent_test_wav[] = {
    'R','I','F','F',44,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
    1,0,1,0,0x80,0x3e,0,0,0,0x7d,0,0,2,0,16,0,'d','a','t','a',8,0,0,0,
    0,1,2,3,4,5,6,7,
};
const uint8_t script_agent_test_mp3[] = {'I','D','3',4,0,0,0,0,0,0,0xff,0xfb,0x90,0};

void script_agent_audio_fixture(char path[160], bool mp3)
{
    strcpy(path, "/tmp/solaros-agent-audio-XXXXXX");
    int fd = mkstemp(path); assert(fd >= 0);
    const void *data = mp3 ? (const void *)script_agent_test_mp3 : (const void *)script_agent_test_wav;
    size_t length = mp3 ? sizeof(script_agent_test_mp3) : sizeof(script_agent_test_wav);
    assert(write(fd, data, length) == (ssize_t)length && !close(fd));
}
