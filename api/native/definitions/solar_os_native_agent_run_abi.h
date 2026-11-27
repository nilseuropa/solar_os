#pragma once

#include "solar_os_native_script_runner_abi.h"

#define SOLAR_OS_NATIVE_AGENT_RUN_SERVICE "agent.run"
#define SOLAR_OS_NATIVE_AGENT_RUN_ABI 1U

#define SOLAR_OS_NATIVE_AGENT_EVENT_STATUS 0U
#define SOLAR_OS_NATIVE_AGENT_EVENT_TEXT_DELTA 1U
#define SOLAR_OS_NATIVE_AGENT_EVENT_TOOL_CALL 2U
#define SOLAR_OS_NATIVE_AGENT_EVENT_TOOL_RESULT 3U
#define SOLAR_OS_NATIVE_AGENT_EVENT_TOOL_CONFIRMATION 4U
#define SOLAR_OS_NATIVE_AGENT_EVENT_USAGE 5U
#define SOLAR_OS_NATIVE_AGENT_EVENT_ERROR 6U
#define SOLAR_OS_NATIVE_AGENT_EVENT_DONE 7U
#define SOLAR_OS_AGENT_EVENT_TEXT_MAX 192U
#define SOLAR_OS_AGENT_AUDIO_BYTES_MAX (1024U * 1024U)
#define SOLAR_OS_AGENT_IMAGE_BYTES_MAX (512U * 1024U)
#define SOLAR_OS_AGENT_IMAGES_MAX 2U
#define SOLAR_OS_AGENT_PROMPT_MAX 1024
#define SOLAR_OS_AGENT_TOOL_NAME_MAX 48U
#define SOLAR_OS_AGENT_SCRIPT_INPUT_SOURCE 0U
#define SOLAR_OS_AGENT_SCRIPT_INPUT_FILE 1U
#define SOLAR_OS_NATIVE_AGENT_SCRIPT_PYTHON (1U << 0)
#define SOLAR_OS_NATIVE_AGENT_SCRIPT_LUA (1U << 1)

typedef struct {
    uint32_t type;
    char text[SOLAR_OS_AGENT_EVENT_TEXT_MAX];
    char tool_name[SOLAR_OS_AGENT_TOOL_NAME_MAX];
    uint32_t prompt_tokens, completion_tokens, total_tokens;
    bool success;
} solar_os_native_agent_event_v1_t;

typedef struct {
    const solar_os_native_handle_t *images;
    size_t image_count;
    solar_os_native_handle_t audio;
} solar_os_native_agent_media_v1_t;

typedef struct {
    const char *prompt;
    solar_os_native_agent_media_v1_t media;
    /* Null is stateless, empty starts a saved conversation, and a nonempty
     * value resumes the given ID. To save a turn, supply next_id to run. */
    const char *conversation_id;
    const char *storage_cwd;
    int (*event)(const solar_os_native_agent_event_v1_t *event, void *user);
    bool (*cancelled)(void *user);
    int (*confirm_tool)(const char *tool_name, const char *risk,
                         const char *arguments, bool *allowed, void *user);
    int (*run_script)(uint32_t language, uint32_t input_type, const char *input,
                       int argc, const char *const *argv, char *output, size_t capacity,
                       solar_os_native_script_result_v1_t *result, void *user);
    uint32_t script_languages;
    void *user;
} solar_os_native_agent_request_v1_t;

typedef struct {
    const char *prompt;
    solar_os_native_agent_media_v1_t media;
    const char *conversation_id;
    const char *storage_cwd;
    bool (*cancelled)(void *user);
    void *user;
} solar_os_native_agent_query_v1_t;

typedef struct {
    size_t text_len;
    bool truncated;
    char conversation_id[24];
    uint32_t prompt_tokens, completion_tokens, total_tokens;
} solar_os_native_agent_answer_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    /* A blocking call on the application's worker. The request, referenced
     * media handles, and callbacks are borrowed through return. Event and
     * script data are borrowed only for each callback on the calling task.
     * next_id is optional. When supplied it must fit a conversation ID and
     * is written only on success. */
    int (*run)(const solar_os_native_agent_request_v1_t *request,
                char *next_id, size_t capacity);
    /* Cancel the currently active run, if any. */
    int (*cancel)(void);
    /* Validate the attachment formats and byte limits without a request.
     * Null media or a zero-length media record specifies no attachments. */
    int (*validate_media)(const solar_os_native_agent_media_v1_t *media);
    /* Collect response text into the supplied, nonempty buffer. The result
     * reports the copied length, truncation, latest usage, and saved ID.
     * The call uses the same policy as the scripting bindings, with no
     * interpreter or confirmation callbacks. On failure, output is cleared. */
    int (*ask)(const solar_os_native_agent_query_v1_t *query, char *text,
               size_t capacity, solar_os_native_agent_answer_v1_t *answer);
} solar_os_native_agent_run_api_v1_t;
