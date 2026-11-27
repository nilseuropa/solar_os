#include "solar_os_native_agent_run.h"
#include "solar_os_config.h"

#if SOLAR_OS_PACKAGE_SERVICE_AGENT
#include "solar_os_agent.h"
#include "solar_os_agent_images.h"
#include "solar_os_agent_tools.h"
#include "solar_os_native_agent_tools_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_script_agent.h"

#include <string.h>

typedef struct {
    solar_os_native_handle_t image_ids[SOLAR_OS_AGENT_IMAGES_MAX];
    solar_os_media_sample_t *images[SOLAR_OS_AGENT_IMAGES_MAX];
    size_t image_count;
    solar_os_native_handle_t audio_id;
    solar_os_media_sample_t *audio;
} agent_media_refs_t;

static void agent_media_release(const agent_media_refs_t *refs)
{
    for (size_t i = 0; i < refs->image_count; i++) {
        solar_os_native_handle_release(refs->image_ids[i]);
    }
    if (refs->audio_id) solar_os_native_handle_release(refs->audio_id);
}

static int agent_media_acquire(const solar_os_native_agent_media_v1_t *media,
                               agent_media_refs_t *refs)
{
    if (!media) return SOLAR_OS_NATIVE_OK;
    if (media->image_count > SOLAR_OS_AGENT_IMAGES_MAX ||
        (media->image_count && !media->images)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    for (size_t i = 0; i < media->image_count; i++) {
        const solar_os_native_handle_t id = media->images[i];
        solar_os_media_sample_t *image = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_MEDIA_SAMPLE);
        if (!image) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
        refs->image_ids[refs->image_count] = id;
        refs->images[refs->image_count++] = image;
    }
    if (media->audio) {
        refs->audio = solar_os_native_handle_acquire(media->audio, SOLAR_OS_NATIVE_HANDLE_MEDIA_SAMPLE);
        if (!refs->audio) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
        refs->audio_id = media->audio;
    }
    return SOLAR_OS_NATIVE_OK;
}

static int agent_validate_media(const solar_os_native_agent_media_v1_t *media)
{
    agent_media_refs_t refs = {0};
    int result = agent_media_acquire(media, &refs);
    if (!result) result = solar_os_native_result(solar_os_agent_images_validate(refs.images, refs.image_count));
    if (!result) result = solar_os_native_result(solar_os_agent_audio_validate(refs.audio));
    agent_media_release(&refs);
    return result;
}

static esp_err_t agent_run_event(const solar_os_agent_event_t *event, void *user)
{
    const solar_os_native_agent_request_v1_t *request = user;
    return solar_os_native_callback_result(request->event(event, request->user));
}

static bool agent_run_cancelled(void *user)
{
    const solar_os_native_agent_request_v1_t *request = user;
    return request->cancelled(request->user);
}

static esp_err_t agent_run_confirm_tool(const char *name, const char *risk, const char *arguments,
                                        bool *allowed, void *user)
{
    const solar_os_native_agent_request_v1_t *request = user;
    *allowed = false;
    return solar_os_native_callback_result(
        request->confirm_tool(name, risk, arguments, allowed, request->user));
}

static esp_err_t agent_run_script(solar_os_agent_script_language_t language,
                                  solar_os_script_input_t input_type, const char *input,
                                  int argc, const char *const *argv, char *output,
                                  size_t capacity, solar_os_script_run_result_t *result,
                                  void *user)
{
    _Static_assert(SOLAR_OS_AGENT_SCRIPT_INPUT_SOURCE == SOLAR_OS_SCRIPT_INPUT_SOURCE &&
                   SOLAR_OS_AGENT_SCRIPT_INPUT_FILE == SOLAR_OS_SCRIPT_INPUT_FILE &&
                   SOLAR_OS_NATIVE_AGENT_SCRIPT_PYTHON == SOLAR_OS_AGENT_SCRIPT_PYTHON &&
                   SOLAR_OS_NATIVE_AGENT_SCRIPT_LUA == SOLAR_OS_AGENT_SCRIPT_LUA,
                   "Agent script callback arguments");
    const solar_os_native_agent_request_v1_t *request = user;
    solar_os_native_script_result_v1_t value = {0};
    int error = request->run_script(language, input_type, input, argc, argv,
                                    output, capacity, &value, request->user);
    if (capacity) output[capacity - 1] = '\0';
    if (error) return solar_os_native_callback_result(error);
    *result = (solar_os_script_run_result_t){
        .status = solar_os_native_callback_result(value.status),
        .success = value.success,
        .cancelled = value.cancelled,
        .timed_out = value.timed_out,
        .output_truncated = value.output_truncated,
        .output_len = value.output_len,
    };
    value.error[sizeof(value.error) - 1] = '\0';
    strlcpy(result->error, value.error, sizeof(result->error));
    return ESP_OK;
}

static int agent_run(const solar_os_native_agent_request_v1_t *in, char *next_id, size_t capacity)
{
    const uint32_t languages = SOLAR_OS_NATIVE_AGENT_SCRIPT_PYTHON | SOLAR_OS_NATIVE_AGENT_SCRIPT_LUA;
    if (!in || !in->event || (!next_id && capacity) ||
        (in->script_languages & ~languages) || (in->script_languages && !in->run_script)) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    if (next_id && capacity < SOLAR_OS_AGENT_CONVERSATION_ID_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;

    solar_os_native_agent_request_v1_t request = *in;
    agent_media_refs_t refs = {0};
    int result = agent_media_acquire(&request.media, &refs);
    if (!result) {
        char id[SOLAR_OS_AGENT_CONVERSATION_ID_MAX] = {0};
        const solar_os_agent_request_t core = {
            .prompt = request.prompt,
            .images = refs.images,
            .image_count = refs.image_count,
            .audio = refs.audio,
            .conversation_id = request.conversation_id,
            .storage_cwd = request.storage_cwd,
            .next_conversation_id = next_id && request.conversation_id ? id : NULL,
            .next_conversation_id_len = sizeof(id),
            .event_handler = agent_run_event,
            .should_cancel = request.cancelled ? agent_run_cancelled : NULL,
            .confirm_tool = request.confirm_tool ? agent_run_confirm_tool : NULL,
            .run_script = request.run_script ? agent_run_script : NULL,
            .script_languages = request.script_languages,
            .user_data = &request,
        };
        result = solar_os_native_result(solar_os_agent_run(&core));
        if (!result && next_id) strlcpy(next_id, id, capacity);
    }
    agent_media_release(&refs);
    return result;
}

static int agent_cancel(void)
{
    return solar_os_native_result(solar_os_agent_cancel());
}

static int agent_ask(const solar_os_native_agent_query_v1_t *query, char *text,
                     size_t capacity, solar_os_native_agent_answer_v1_t *answer)
{
    if (!query || !text || !capacity || !answer) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    agent_media_refs_t refs = {0};
    int result = agent_media_acquire(&query->media, &refs);
    if (!result) {
        const solar_os_agent_request_t request = {
            .prompt = query->prompt,
            .images = refs.images,
            .image_count = refs.image_count,
            .audio = refs.audio,
            .conversation_id = query->conversation_id,
            .storage_cwd = query->storage_cwd,
            .should_cancel = query->cancelled,
            .user_data = query->user,
        };
        result = solar_os_native_result(
            solar_os_script_agent_capture_answer(&request, text, capacity, answer));
    } else {
        *answer = (solar_os_native_agent_answer_v1_t){0};
        text[0] = '\0';
    }
    agent_media_release(&refs);
    return result;
}

static const solar_os_native_agent_run_api_v1_t agent_run_api = {
    .abi_version = SOLAR_OS_NATIVE_AGENT_RUN_ABI,
    .struct_size = sizeof(agent_run_api),
    .run = agent_run,
    .cancel = agent_cancel,
    .validate_media = agent_validate_media,
    .ask = agent_ask,
};
_Static_assert(SOLAR_OS_NATIVE_AGENT_RISK_READ_ONLY == SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY &&
               SOLAR_OS_NATIVE_AGENT_RISK_SENSITIVE_READ == SOLAR_OS_AGENT_TOOL_RISK_SENSITIVE_READ &&
               SOLAR_OS_NATIVE_AGENT_RISK_MUTATING == SOLAR_OS_AGENT_TOOL_RISK_MUTATING &&
               SOLAR_OS_NATIVE_AGENT_RISK_DISRUPTIVE == SOLAR_OS_AGENT_TOOL_RISK_DISRUPTIVE &&
               SOLAR_OS_NATIVE_AGENT_TOOL_DENY == SOLAR_OS_AGENT_TOOL_POLICY_DENY &&
               SOLAR_OS_NATIVE_AGENT_TOOL_ALLOW == SOLAR_OS_AGENT_TOOL_POLICY_ALLOW &&
               SOLAR_OS_NATIVE_AGENT_TOOL_CONFIRM_ONCE == SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM_ONCE,
               "Agent tool risk and decision values");

static int agent_tool_describe(size_t index, solar_os_native_agent_tool_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_agent_tool_info_v1_t){0};
    solar_os_agent_tool_info_t info;
    if (!solar_os_agent_tools_get(index, &info)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *out = (solar_os_native_agent_tool_info_v1_t){
        .name = info.provider.name,
        .description = info.provider.description,
        .domain = info.domain,
        .arguments_json_schema = info.provider.parameters_json,
        .output_json_schema = info.output_schema_json,
        .required_capability = info.required_capability,
        .risk = info.risk,
        .available = info.available,
        .strict = info.provider.strict,
    };
    return SOLAR_OS_NATIVE_OK;
}

static uint32_t agent_tool_policy_decision(uint32_t policy, uint32_t risk)
{
    if (policy > SOLAR_OS_NATIVE_AGENT_POLICY_ALL || risk > SOLAR_OS_NATIVE_AGENT_RISK_DISRUPTIVE)
        return SOLAR_OS_NATIVE_AGENT_TOOL_DENY;
    return solar_os_agent_tools_policy_decision(policy, risk);
}

static const char *agent_tool_risk_name(uint32_t risk)
{
    return risk <= SOLAR_OS_NATIVE_AGENT_RISK_DISRUPTIVE ?
        solar_os_agent_tool_risk_name(risk) : NULL;
}

static int agent_tool_invoke(const char *name, const char *arguments,
    const solar_os_native_agent_tool_options_v1_t *options, char *out, size_t capacity)
{
    if (!out || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const uint32_t languages = SOLAR_OS_NATIVE_AGENT_SCRIPT_PYTHON | SOLAR_OS_NATIVE_AGENT_SCRIPT_LUA;
    solar_os_native_agent_request_v1_t callbacks = {0};
    if (options) {
        if ((options->script_languages & ~languages) ||
            (options->script_languages && !options->run_script)) {
            return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        }
        callbacks.confirm_tool = options->confirm_tool;
        callbacks.run_script = options->run_script;
        callbacks.script_languages = options->script_languages;
        callbacks.storage_cwd = options->storage_cwd;
        callbacks.user = options->user;
    }
    const solar_os_agent_request_t request = {
        .confirm_tool = callbacks.confirm_tool ? agent_run_confirm_tool : NULL,
        .run_script = callbacks.run_script ? agent_run_script : NULL,
        .script_languages = callbacks.script_languages,
        .storage_cwd = callbacks.storage_cwd,
        .user_data = &callbacks,
    };
    const esp_err_t err = solar_os_agent_invoke_tool(name, arguments, &request, out, capacity);
    if (err != ESP_OK) *out = '\0';
    return err == ESP_ERR_NOT_ALLOWED ? SOLAR_OS_NATIVE_ERROR_PERMISSION : solar_os_native_result(err);
}

static const solar_os_native_agent_tools_api_v1_t agent_tools_api = {
    .abi_version = SOLAR_OS_NATIVE_AGENT_TOOLS_ABI,
    .struct_size = sizeof(agent_tools_api),
    .count = solar_os_agent_tools_count,
    .describe = agent_tool_describe,
    .policy_decision = agent_tool_policy_decision,
    .risk_name = agent_tool_risk_name,
    .invoke = agent_tool_invoke,
};
#endif

const void *solar_os_native_agent_run_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_agent_run_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
