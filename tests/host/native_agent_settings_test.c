#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "solar_os_native_agent.h"
#include "solar_os_native_agent_run.h"
#include "solar_os_native_agent_tools_abi.h"
#include "solar_os_storage.h"
#include "solar_os_native_handles.h"

#define MALLOC_CAP_INTERNAL 4U
static size_t heap_caps_get_free_size(uint32_t caps) { return caps << 12U; }
static size_t heap_caps_get_largest_free_block(uint32_t caps) { return caps << 10U; }

#include "solar_os_agent.c"

typedef struct {
    solar_os_agent_provider_config_t config;
    char policy[16];
    uint8_t max_tools;
} saved_settings_t;

static saved_settings_t saved = {
    .config = {
        .endpoint = "https://provider.invalid",
        .model = "original",
        .api_key = "test-only-key",
        .reasoning_effort = "medium",
    },
    .policy = "confirm",
    .max_tools = 8,
};
static saved_settings_t staged;
static bool fail_commit;
static unsigned loading_count, open_handles;
static pthread_barrier_t first_calls;
static const solar_os_native_agent_api_v1_t *api;

static esp_err_t fixture_provider(const solar_os_agent_provider_config_t *config,
                                   const solar_os_agent_provider_turn_t *turn,
                                   solar_os_agent_event_fn event, void *user,
                                   solar_os_agent_provider_result_t *result);
const solar_os_agent_provider_t solar_os_agent_openai_provider = {
    .name = "fixture", .run_turn = fixture_provider,
};

size_t strlcpy(char *out, const char *in, size_t capacity)
{
    const size_t len = strlen(in);
    if (capacity != 0) {
        const size_t copied = len < capacity ? len : capacity - 1;
        memcpy(out, in, copied);
        out[copied] = '\0';
    }
    return len;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, "agent") && !open_handles);
    open_handles++;
    *handle = mode == NVS_READONLY ? 1U : 2U;
    if (mode == NVS_READONLY) {
        loading_count++;
    } else {
        staged = saved;
    }
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert((handle == 1 || handle == 2) && open_handles == 1);
    open_handles--;
}

static char *setting_string(saved_settings_t *settings, const char *key, size_t *capacity)
{
#define SETTING(key_name, field) \
    if (!strcmp(key, key_name)) { \
        *capacity = sizeof(settings->field); \
        return settings->field; \
    }
    SETTING("endpoint", config.endpoint)
    SETTING("model", config.model)
    SETTING("api_key", config.api_key)
    SETTING("reasoning", config.reasoning_effort)
    SETTING("tool_policy", policy)
#undef SETTING
    assert(0);
    return NULL;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *len)
{
    assert(handle == 1);
    size_t capacity = 0;
    const char *src = setting_string(&saved, key, &capacity);
    assert(*len == capacity);
    *len = strlcpy(value, src, capacity) + 1U;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    assert(handle == 2);
    size_t capacity = 0;
    char *out = setting_string(&staged, key, &capacity);
    assert(strlcpy(out, value, capacity) < capacity);
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out)
{
    assert(handle == 1 && !strcmp(key, "max_tools"));
    *out = saved.max_tools;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(handle == 2 && !strcmp(key, "max_tools"));
    staged.max_tools = value;
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    assert(handle == 2);
    memset(&staged, 0, sizeof(staged));
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 2);
    if (fail_commit) return ESP_FAIL;
    saved = staged;
    return ESP_OK;
}

/* The reference corpus has its own test suite. Check the native boundary's
 * forwarding and clearing policy here. */
esp_err_t solar_os_agent_reference_search(const char *query, char *out, size_t capacity)
{
    assert(!strcmp(query, "stream"));
    return (size_t)snprintf(out, capacity, "{\"matches\": []}") < capacity ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void *read_settings(void *unused)
{
    (void)unused;
    (void)pthread_barrier_wait(&first_calls);
    solar_os_native_agent_status_v1_t status;
    assert(!api->status(&status) && status.initialized && status.api_key_set);
    assert(status.configured && !strcmp(status.provider, "fixture"));
    assert(status.last_http_status == -1 && status.max_tools == 8);
    return NULL;
}

static char store_root[] = "/tmp/solaros-agent-store-XXXXXX";
static bool fail_publish;
static atomic_size_t allocations;

bool solar_os_storage_sd_is_mounted(void) { return false; }

esp_err_t solar_os_storage_default_path(const char *relative, char *out, size_t capacity)
{
    const int written = snprintf(out, capacity, "%s/%s", store_root, relative);
    return written >= 0 && (size_t)written < capacity ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t solar_os_storage_mkdir(const char *path)
{
    return mkdir(path, 0700) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t solar_os_storage_remove(const char *path)
{
    return remove(path) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t solar_os_storage_rename(const char *old, const char *new)
{
    if (fail_publish && strstr(old, ".tmp")) {
        errno = EIO;
        return ESP_FAIL;
    }
    return rename(old, new) == 0 ? ESP_OK : ESP_FAIL;
}

void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t memory_class,
                            const char *tag)
{
    assert(bytes && memory_class == SOLAR_OS_MEMORY_EXTERNAL_REQUIRED && tag);
    void *allocation = malloc(bytes);
    if (allocation) allocations++;
    return allocation;
}

void *solar_os_memory_calloc(size_t count, size_t bytes,
                              solar_os_memory_class_t memory_class, const char *tag)
{
    void *allocation = solar_os_memory_alloc(count * bytes, memory_class, tag);
    if (allocation) memset(allocation, 0, count * bytes);
    return allocation;
}

void solar_os_memory_free(void *allocation)
{
    if (allocation) {
        assert(allocations);
        allocations--;
        free(allocation);
    }
}

static int visit_message(const solar_os_native_agent_message_v1_t *message, void *user)
{
    size_t *visited = user;
    static const char *const text[] = {"first prompt", "one tool", "first answer"};
    static const uint32_t roles[] = {
        SOLAR_OS_NATIVE_AGENT_MESSAGE_USER, SOLAR_OS_NATIVE_AGENT_MESSAGE_TOOL,
        SOLAR_OS_NATIVE_AGENT_MESSAGE_ASSISTANT,
    };
    assert(*visited < 3 && !strcmp(message->text, text[*visited]));
    assert(message->role == roles[*visited] && message->length == strlen(text[*visited]));
    (*visited)++;
    return *visited == 3 ? SOLAR_OS_NATIVE_ERROR_BUSY : SOLAR_OS_NATIVE_OK;
}

static int visit_history(const solar_os_native_agent_history_v1_t *history, void *user)
{
    const bool with_messages = *(const bool *)user;
    assert(!strcmp(history->provider_response_id, "response1"));
    if (with_messages) {
        assert(history->count == 3 && history->messages);
        size_t visited = 0;
        for (size_t i = 0; i < history->count; i++) visit_message(&history->messages[i], &visited);
        assert(visited == 3);
    } else {
        assert(!history->count && !history->messages);
    }
    return SOLAR_OS_NATIVE_ERROR_BUSY;
}

typedef struct {
    const solar_os_native_agent_conversation_api_v1_t *conversation;
    solar_os_native_agent_conversation_turn_v1_t turn;
} append_request_t;

static void *append_turn(void *user)
{
    const append_request_t *request = user;
    char id[SOLAR_OS_AGENT_CONVERSATION_ID_MAX];
    assert(!request->conversation->commit(&request->turn, id, sizeof(id)));
    assert(!strcmp(id, request->turn.id));
    return NULL;
}

static void test_conversations(void)
{
    const solar_os_native_agent_conversation_api_v1_t *conversation =
        solar_os_native_agent_get_service("agent.conversation", 1, sizeof(*conversation));
    assert(conversation && mkdtemp(store_root));
    size_t count = 99;
    assert(!conversation->list(NULL, 0, &count) && !count);
    char id[SOLAR_OS_AGENT_CONVERSATION_ID_MAX];
    solar_os_native_agent_conversation_turn_v1_t turn = {
        .provider = "fixture", .model = "model",
        .user_text = "first prompt", .assistant_text = "first answer",
        .tool_summary = "one tool", .provider_response_id = "response1",
    };
    assert(!conversation->commit(&turn, id, sizeof(id)) && id[0]);
    solar_os_native_agent_conversation_info_v1_t summary;
    assert(!conversation->list(&summary, 1, &count) && count == 1);
    assert(!strcmp(summary.id, id) && summary.turn_count == 1 && summary.stored_bytes);

    solar_os_agent_conversation_info_t raw;
    assert(!solar_os_agent_conversation_get(id, &raw) && !strcmp(raw.title, "first prompt"));
    assert(!conversation->get(id, &summary) && !strcmp(raw.title, summary.title));
    size_t visited = 0;
    assert(conversation->visit(id, visit_message, &visited) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(visited == 3 && !allocations);
    const bool with_messages = true, without_messages = false;
    assert(conversation->with_history(id, true, visit_history, (void *)&with_messages) ==
           SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(conversation->with_history(id, false, visit_history, (void *)&without_messages) ==
           SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(!allocations);

    turn.id = id;
    turn.tool_summary = NULL;
    turn.user_text = "follow-up";
    turn.assistant_text = "response";
    fail_publish = true;
    char repeated[SOLAR_OS_AGENT_CONVERSATION_ID_MAX];
    assert(conversation->commit(&turn, repeated, sizeof(repeated)) == SOLAR_OS_NATIVE_ERROR_FAILED);
    fail_publish = false;
    assert(!conversation->get(id, &summary) && summary.turn_count == 1);

    const append_request_t request = {.conversation = conversation, .turn = turn};
    pthread_t writers[2];
    for (size_t i = 0; i < 2; i++) assert(!pthread_create(&writers[i], NULL, append_turn, (void *)&request));
    for (size_t i = 0; i < 2; i++) assert(!pthread_join(writers[i], NULL));
    assert(!conversation->get(id, &summary) && summary.turn_count == 3);
    assert(!conversation->remove(id));
    assert(conversation->get(id, &summary) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !summary.id[0]);
    assert(!conversation->list(NULL, 0, &count) && !count && !allocations);
    const char *const dirs[] = {".solar/agent/conversations", ".solar/agent", ".solar"};
    for (size_t i = 0; i < 3; i++) {
        char path[SOLAR_OS_STORAGE_PATH_MAX];
        assert(!solar_os_storage_default_path(dirs[i], path, sizeof(path)) && !rmdir(path));
    }
    assert(!rmdir(store_root));
}

static bool tool_run, report_usage;
static unsigned transport_cancels;
static int transport_token;

const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_OK ? "ok" : "fixture failure";
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *format, ...)
{
    (void)level;
    (void)format;
    assert(!strcmp(tag, "agent"));
    return ESP_OK;
}

esp_err_t solar_os_http_request_cancel(solar_os_http_request_t *request)
{
    assert(request == (void *)&transport_token);
    transport_cancels++;
    return ESP_OK;
}

static const solar_os_agent_tool_descriptor_t fixture_tool = {.name = "script_fixture"};

size_t solar_os_agent_tools_collect(const solar_os_agent_request_t *request,
    solar_os_agent_tool_policy_t policy, solar_os_agent_tool_descriptor_t *descriptors, size_t capacity)
{
    if (!tool_run) return 0;
    assert(request->run_script && request->script_languages == SOLAR_OS_AGENT_SCRIPT_PYTHON);
    assert(policy == SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM && capacity >= 1);
    descriptors[0] = fixture_tool;
    return 1;
}

size_t solar_os_agent_tools_count(void) { return tool_run ? 1 : 0; }

bool solar_os_agent_tools_get(size_t index, solar_os_agent_tool_info_t *info)
{
    if (index || !tool_run) return false;
    *info = (solar_os_agent_tool_info_t){
        .provider = fixture_tool, .risk = SOLAR_OS_AGENT_TOOL_RISK_MUTATING,
    };
    return true;
}

bool solar_os_agent_tools_is_discovery(const char *name)
{
    assert(!strcmp(name, fixture_tool.name));
    return false;
}

size_t solar_os_agent_tools_activate(const char *name, const solar_os_agent_request_t *request,
    solar_os_agent_tool_policy_t policy, solar_os_agent_tool_descriptor_t *descriptors,
    size_t count, size_t capacity)
{
    (void)name; (void)request; (void)policy; (void)descriptors; (void)count; (void)capacity;
    assert(0);
    return 0;
}

size_t solar_os_agent_tools_collect_discovered(const char *arguments, const solar_os_agent_request_t *request,
    solar_os_agent_tool_policy_t policy, solar_os_agent_tool_descriptor_t *descriptors, size_t capacity)
{
    (void)arguments; (void)request; (void)policy; (void)descriptors; (void)capacity;
    assert(0);
    return 0;
}

esp_err_t solar_os_agent_tools_execute(const char *name, const char *arguments,
    const solar_os_agent_request_t *request, solar_os_agent_tool_policy_t policy,
    bool confirmed, char *result, size_t capacity)
{
    assert(!strcmp(name, fixture_tool.name) && !strcmp(arguments, "{}"));
    assert(policy == SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM && confirmed);
    char output[8] = {0};
    solar_os_script_run_result_t run = {0};
    assert(!request->run_script(SOLAR_OS_AGENT_SCRIPT_PYTHON, SOLAR_OS_SCRIPT_INPUT_SOURCE,
        "fixture", 0, NULL, output, sizeof(output), &run, request->user_data));
    assert(output[7] == 0 && strlen(run.error) == sizeof(run.error) - 1);
    assert(run.success && run.status == ESP_OK && run.output_len == 7);
    assert(strlcpy(result, "{\"script\":true}", capacity) < capacity);
    return ESP_OK;
}

static esp_err_t fixture_provider(const solar_os_agent_provider_config_t *config,
                                   const solar_os_agent_provider_turn_t *turn,
                                   solar_os_agent_event_fn event, void *user,
                                   solar_os_agent_provider_result_t *result)
{
    assert(!strcmp(config->model, "run-fixture") && !strcmp(turn->prompt, "describe"));
    assert(turn->image_count == 1 && solar_os_media_sample_image(turn->images[0]));
    result->http_status = 200;
    if (tool_run && !turn->continuation) {
        assert(turn->tool_count == 1 && !strcmp(turn->tools[0].name, fixture_tool.name));
        result->tool_call = true;
        strcpy(result->tool_name, fixture_tool.name);
        strcpy(result->tool_call_id, "call");
        strcpy(result->tool_arguments, "{}");
        return ESP_OK;
    }
    if (tool_run) assert(turn->continuation && !strcmp(turn->tool_result, "{\"script\":true}"));
    solar_os_http_request_t *transport = (void *)&transport_token;
    solar_os_agent_provider_bind_request(transport);
    const solar_os_agent_event_t response = {.type = SOLAR_OS_AGENT_EVENT_TEXT_DELTA, .text = "answer"};
    esp_err_t error = event(&response, user);
    if (error == ESP_OK && report_usage) {
        const solar_os_agent_event_t usage = {
            .type = SOLAR_OS_AGENT_EVENT_USAGE,
            .prompt_tokens = 3, .completion_tokens = 8, .total_tokens = 11,
        };
        error = event(&usage, user);
    }
    if (solar_os_agent_provider_cancel_requested()) error = ESP_ERR_INVALID_STATE;
    solar_os_agent_provider_unbind_request(transport);
    return error;
}

typedef struct {
    const solar_os_native_agent_run_api_v1_t *run;
    solar_os_native_handle_t image;
    bool cancel_on_text, cancelled, deny_tool;
    unsigned events, confirmations, scripts;
} run_fixture_t;

static int run_event(const solar_os_native_agent_event_v1_t *event, void *user)
{
    run_fixture_t *fixture = user;
    assert(solar_os_native_handle_close_kind(fixture->image, SOLAR_OS_NATIVE_HANDLE_MEDIA_SAMPLE) ==
           SOLAR_OS_NATIVE_ERROR_BUSY);
    fixture->events++;
    if (event->type == SOLAR_OS_NATIVE_AGENT_EVENT_TEXT_DELTA) {
        assert(!strcmp(event->text, "answer"));
        if (fixture->cancel_on_text) {
            fixture->cancelled = true;
            assert(!fixture->run->cancel());
        }
    }
    if (event->type == SOLAR_OS_NATIVE_AGENT_EVENT_DONE) {
        assert(event->success != fixture->cancelled);
    }
    return SOLAR_OS_NATIVE_OK;
}

static bool run_cancelled(void *user) { return ((run_fixture_t *)user)->cancelled; }

static int run_confirm(const char *name, const char *risk, const char *arguments,
                      bool *allowed, void *user)
{
    run_fixture_t *fixture = user;
    assert(!strcmp(name, fixture_tool.name) && !strcmp(risk, "mutating") && !strcmp(arguments, "{}"));
    assert(!*allowed);
    fixture->confirmations++;
    *allowed = !fixture->deny_tool;
    return SOLAR_OS_NATIVE_OK;
}

static int run_script(uint32_t language, uint32_t input_type, const char *input,
                      int argc, const char *const *argv, char *output, size_t capacity,
                      solar_os_native_script_result_v1_t *result, void *user)
{
    run_fixture_t *fixture = user;
    assert(language == SOLAR_OS_NATIVE_AGENT_SCRIPT_PYTHON && input_type == SOLAR_OS_AGENT_SCRIPT_INPUT_SOURCE);
    assert(!strcmp(input, "fixture") && !argc && !argv);
    fixture->scripts++;
    memset(output, 'o', capacity);
    memset(result->error, 'e', sizeof(result->error));
    result->success = true;
    result->output_len = capacity - 1;
    return SOLAR_OS_NATIVE_OK;
}

static int destroy_media(void *value)
{
    solar_os_media_sample_release(value);
    return SOLAR_OS_NATIVE_OK;
}

static void test_tool_invocation(void)
{
    const solar_os_native_agent_tools_api_v1_t *tools =
        solar_os_native_agent_run_get_service("agent.tools", 1, sizeof(*tools));
    tool_run = true;
    assert(tools && tools->count() == 1);
    solar_os_native_agent_tool_info_v1_t info;
    assert(!tools->describe(0, &info) && !strcmp(info.name, fixture_tool.name));
    assert(info.risk == SOLAR_OS_NATIVE_AGENT_RISK_MUTATING);
    assert(!strcmp(tools->risk_name(info.risk), "mutating"));
    assert(tools->describe(1, &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !info.name);
    assert(tools->policy_decision(SOLAR_OS_NATIVE_AGENT_POLICY_CONFIRM, SOLAR_OS_NATIVE_AGENT_RISK_READ_ONLY) ==
           SOLAR_OS_NATIVE_AGENT_TOOL_ALLOW);
    assert(tools->policy_decision(SOLAR_OS_NATIVE_AGENT_POLICY_CONFIRM, SOLAR_OS_NATIVE_AGENT_RISK_MUTATING) ==
           SOLAR_OS_NATIVE_AGENT_TOOL_CONFIRM_ONCE);
    assert(tools->policy_decision(SOLAR_OS_NATIVE_AGENT_POLICY_READ_ONLY, SOLAR_OS_NATIVE_AGENT_RISK_MUTATING) ==
           SOLAR_OS_NATIVE_AGENT_TOOL_DENY);
    assert(tools->policy_decision(255, SOLAR_OS_NATIVE_AGENT_RISK_MUTATING) == SOLAR_OS_NATIVE_AGENT_TOOL_DENY);

    run_fixture_t fixture = {0};
    const solar_os_native_agent_tool_options_v1_t options = {
        .confirm_tool = run_confirm, .run_script = run_script,
        .script_languages = SOLAR_OS_NATIVE_AGENT_SCRIPT_PYTHON, .user = &fixture,
    };
    char output[64];
    assert(!api->set_tool_policy(SOLAR_OS_NATIVE_AGENT_POLICY_OFF));
    assert(tools->invoke(fixture_tool.name, "{}", &options, output, sizeof(output)) ==
           SOLAR_OS_NATIVE_ERROR_PERMISSION);
    assert(!output[0] && !fixture.confirmations && !fixture.scripts);
    assert(!api->set_tool_policy(SOLAR_OS_NATIVE_AGENT_POLICY_CONFIRM));
    assert(tools->invoke(fixture_tool.name, "{}", NULL, output, sizeof(output)) ==
           SOLAR_OS_NATIVE_ERROR_PERMISSION);
    assert(!output[0] && !fixture.confirmations && !fixture.scripts);
    fixture.deny_tool = true;
    assert(tools->invoke(fixture_tool.name, "{}", &options, output, sizeof(output)) ==
           SOLAR_OS_NATIVE_ERROR_PERMISSION);
    assert(!output[0] && fixture.confirmations == 1 && !fixture.scripts);
    fixture.deny_tool = false;
    assert(!tools->invoke(fixture_tool.name, "{}", &options, output, sizeof(output)));
    assert(!strcmp(output, "{\"script\":true}") && fixture.confirmations == 2 && fixture.scripts == 1);
    tool_run = false;
}

static void test_agent_runs(void)
{
    const solar_os_native_agent_run_api_v1_t *run = solar_os_native_agent_run_get_service(
        "agent.run", 1, sizeof(*run));
    assert(run && !run->validate_media(NULL));
    assert(!api->set_endpoint("https://provider.invalid") && !api->set_model("run-fixture"));
    static const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0x00};
    solar_os_media_sample_t *sample;
    assert(!solar_os_media_sample_copy_image(jpeg, sizeof(jpeg), NULL, &sample));
    run_fixture_t fixture = {.run = run};
    assert(!solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_MEDIA_SAMPLE, sample,
        destroy_media, &fixture.image));
    const solar_os_native_handle_t partial[] = {fixture.image, 0};
    assert(run->validate_media(&(solar_os_native_agent_media_v1_t){
        .images = partial, .image_count = 2,
    }) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(run->validate_media(&(solar_os_native_agent_media_v1_t){
        .audio = fixture.image,
    }) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    solar_os_native_agent_request_v1_t request = {
        .prompt = "describe", .media = {.images = &fixture.image, .image_count = 1},
        .event = run_event, .cancelled = run_cancelled, .user = &fixture,
    };
    assert(!run->validate_media(&request.media));
    char id[SOLAR_OS_AGENT_CONVERSATION_ID_MAX] = "unchanged";
    assert(run->run(&request, id, 1) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE && !fixture.events);
    fixture.cancel_on_text = true;
    assert(run->run(&request, id, sizeof(id)) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(fixture.events == 3 && transport_cancels == 1 && !strcmp(id, "unchanged"));
    fixture.cancelled = fixture.cancel_on_text = false;
    assert(!run->run(&request, id, sizeof(id)) && !id[0] && fixture.events == 6);

    tool_run = true;
    request.confirm_tool = run_confirm;
    request.run_script = run_script;
    request.script_languages = SOLAR_OS_NATIVE_AGENT_SCRIPT_PYTHON;
    assert(!run->run(&request, NULL, 0));
    assert(fixture.confirmations == 1 && fixture.scripts == 1 && fixture.events > 8);
    tool_run = false;
    report_usage = true;
    const solar_os_native_agent_query_v1_t query = {
        .prompt = "describe", .media = request.media,
        .cancelled = run_cancelled, .user = &fixture,
    };
    solar_os_native_agent_answer_v1_t answer;
    char captured[4];
    assert(!run->ask(&query, captured, sizeof(captured), &answer));
    assert(!strcmp(captured, "ans") && answer.text_len == 3 && answer.truncated);
    assert(answer.prompt_tokens == 3 && answer.completion_tokens == 8 && answer.total_tokens == 11);
    fixture.cancelled = true;
    assert(run->ask(&query, captured, sizeof(captured), &answer) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!captured[0] && !answer.text_len && !answer.total_tokens);

    assert(!solar_os_native_handle_close_kind(fixture.image, SOLAR_OS_NATIVE_HANDLE_MEDIA_SAMPLE));
    assert(!allocations && !solar_os_native_handle_count());
}

int main(void)
{
    api = solar_os_native_agent_get_service("agent", 1, sizeof(*api));
    assert(api && !solar_os_native_agent_get_service("agent", 2, 0));
    assert(api->status(NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!pthread_barrier_init(&first_calls, NULL, 3));
    pthread_t readers[2];
    for (size_t i = 0; i < 2; i++) assert(!pthread_create(&readers[i], NULL, read_settings, NULL));
    (void)pthread_barrier_wait(&first_calls);
    assert(!api->set_model("active"));
    for (size_t i = 0; i < 2; i++) assert(!pthread_join(readers[i], NULL));
    assert(!pthread_barrier_destroy(&first_calls) && loading_count == 1);

    assert(api->set_endpoint("not-a-url") == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->set_max_tools(256) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->set_tool_policy(255) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->set_endpoint("https://another.invalid"));
    assert(!api->set_reasoning_effort("low"));
    uint32_t policy;
    assert(!api->parse_tool_policy("readonly", &policy) && policy == SOLAR_OS_NATIVE_AGENT_POLICY_READ_ONLY);
    assert(!strcmp(api->tool_policy_name(policy), "readonly"));
    assert(!api->set_tool_policy(policy) && !api->set_max_tools(12));
    assert(!api->set_api_key("") && !saved.config.api_key[0]);

    solar_os_agent_request_t request = {0};
    solar_os_agent_provider_config_t config;
    solar_os_agent_tool_policy_t run_policy;
    uint8_t max_tools;
    assert(!agent_begin_run(&request, &config, &run_policy, &max_tools));
    assert(!strcmp(config.model, "active") && max_tools == 12 && run_policy == SOLAR_OS_AGENT_TOOL_POLICY_READONLY);
    assert(!strcmp(config.endpoint, "https://another.invalid") && !strcmp(config.reasoning_effort, "low"));
    assert(agent_begin_run(&request, &config, &run_policy, &max_tools) == ESP_ERR_INVALID_STATE);
    assert(api->forget() == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(!api->set_model("next") && !strcmp(config.model, "active"));
    solar_os_native_agent_status_v1_t status;
    assert(!api->status(&status) && status.running && status.request_count == 1);
    assert(status.last_max_tools == 12 && !status.api_key_set && !strcmp(status.model, "next"));
    agent_finish_request(ESP_ERR_TIMEOUT, NULL);
    assert(!api->status(&status) && !status.running && status.failure_count == 1);
    assert(status.last_error == SOLAR_OS_NATIVE_ERROR_TIMEOUT);

    fail_commit = true;
    assert(api->set_model("unsaved") == SOLAR_OS_NATIVE_ERROR_FAILED);
    assert(api->forget() == SOLAR_OS_NATIVE_ERROR_FAILED);
    assert(!api->status(&status) && status.configured && !strcmp(status.model, "next"));
    assert(!strcmp(saved.config.model, "next"));
    fail_commit = false;
    assert(!api->forget());
    assert(!api->status(&status) && !status.configured && !status.api_key_set);
    assert(status.max_tools == SOLAR_OS_AGENT_DEFAULT_MAX_TOOLS && !strcmp(status.reasoning_effort, "medium"));
    assert(status.tool_policy == SOLAR_OS_NATIVE_AGENT_POLICY_CONFIRM && !saved.config.model[0]);
    char json[64];
    assert(api->reference_search("stream", json, 3) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE && !json[0]);
    assert(!api->reference_search("stream", json, sizeof(json)) && !strcmp(json, "{\"matches\": []}"));
    assert(!open_handles);
    test_conversations();
    test_agent_runs();
    test_tool_invocation();
    puts("native Agent settings, conversation storage, run callbacks and media lifetimes passed");
    return 0;
}
