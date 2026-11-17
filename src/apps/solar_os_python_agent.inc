/* Python values are converted here; the shared adapter owns request lifetime. */
#if SOLAR_OS_PACKAGE_SERVICE_AGENT
#include "solar_os_script_agent.h"

static const char *python_agent_string(mp_obj_t value)
{
    size_t length;
    const char *text = mp_obj_str_get_data(value, &length);
    if (strlen(text) != length) mp_raise_ValueError(MP_ERROR_TEXT("embedded NUL in agent argument"));
    return text;
}

static mp_obj_t solaros_agent_status(void)
{
    solar_os_agent_status_t status;
    python_check_esp(solar_os_agent_get_status(&status));
    mp_obj_t result = mp_obj_new_dict(28);
    python_dict_store_cstr(result, "provider", status.provider);
    python_dict_store_cstr(result, "endpoint", status.endpoint);
    python_dict_store_cstr(result, "model", status.model);
    python_dict_store_cstr(result, "reasoning_effort", status.reasoning_effort);
    python_dict_store_bool(result, "initialized", status.initialized);
    python_dict_store_bool(result, "configured", status.configured);
    python_dict_store_bool(result, "api_key_set", status.api_key_set);
    python_dict_store_bool(result, "running", status.running);
    python_dict_store_uint(result, "max_tools", status.max_tools);
    python_dict_store_uint(result, "last_tool_call_count", status.last_tool_call_count);
    python_dict_store_uint(result, "last_max_tools", status.last_max_tools);
    python_dict_store_uint(result, "request_count", status.request_count);
    python_dict_store_uint(result, "failure_count", status.failure_count);
    python_dict_store_uint(result, "tool_executed_count", status.tool_executed_count);
    python_dict_store_uint(result, "tool_denied_count", status.tool_denied_count);
    python_dict_store_uint(result, "tool_failed_count", status.tool_failed_count);
    python_dict_store_uint(result, "last_duration_ms", status.last_duration_ms);
    python_dict_store_uint(result, "last_bytes_received", status.last_bytes_received);
    python_dict_store_uint(result, "last_internal_before", status.last_internal_before);
    python_dict_store_uint(result, "last_internal_low", status.last_internal_low);
    python_dict_store_uint(result, "last_internal_after", status.last_internal_after);
    python_dict_store_uint(result, "last_internal_largest_before", status.last_internal_largest_before);
    python_dict_store_uint(result, "last_internal_largest_after", status.last_internal_largest_after);
    python_dict_store_uint(result, "last_psram_before", status.last_psram_before);
    python_dict_store_uint(result, "last_psram_after", status.last_psram_after);
    python_dict_store_int(result, "last_http_status", status.last_http_status);
    python_dict_store_int(result, "last_error", status.last_error);
    python_dict_store_cstr(result, "tool_policy", solar_os_agent_tool_policy_name(status.tool_policy));
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_agent_status_obj, solaros_agent_status);

static mp_obj_t solaros_agent_ask(size_t n_args, const mp_obj_t *args)
{
    solar_os_script_agent_request_t request = {
        .prompt = python_agent_string(args[0]), .should_cancel = python_should_cancel,
    };
    if (n_args > 1 && args[1] != mp_const_none) {
        mp_obj_t single = args[1], *images = &single;
        size_t count = 1;
        if (!mp_obj_is_str(single)) mp_obj_get_array(single, &count, &images);
        if (count > SOLAR_OS_AGENT_IMAGES_MAX) mp_raise_ValueError(MP_ERROR_TEXT("at most two agent images"));
        request.image_count = count;
        for (size_t i = 0; i < count; i++) request.images[i] = python_agent_string(images[i]);
    }
    if (n_args > 2 && args[2] != mp_const_none) request.conversation_id = python_agent_string(args[2]);
    solar_os_script_agent_result_t response = {0};
    python_check_esp(solar_os_script_agent_ask(&request, &response));
    /* Native PSRAM must be released even if MicroPython allocation raises. */
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t result = mp_obj_new_dict(6);
        python_dict_store_cstr(result, "text", response.text);
        python_dict_store_cstr(result, "conversation_id", response.conversation_id);
        python_dict_store_bool(result, "truncated", response.truncated);
        python_dict_store_uint(result, "prompt_tokens", response.prompt_tokens);
        python_dict_store_uint(result, "completion_tokens", response.completion_tokens);
        python_dict_store_uint(result, "total_tokens", response.total_tokens);
        nlr_pop();
        solar_os_script_agent_result_clear(&response);
        return result;
    }
    solar_os_script_agent_result_clear(&response);
    nlr_jump(nlr.ret_val);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_agent_ask_obj, 1, 3, solaros_agent_ask);

static mp_obj_t solaros_agent_set_endpoint(mp_obj_t value)
{
    python_check_esp(solar_os_agent_set_endpoint(python_agent_string(value)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_agent_set_endpoint_obj, solaros_agent_set_endpoint);

static mp_obj_t solaros_agent_set_model(mp_obj_t value)
{
    python_check_esp(solar_os_agent_set_model(python_agent_string(value)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_agent_set_model_obj, solaros_agent_set_model);

static mp_obj_t solaros_agent_set_api_key(mp_obj_t value)
{
    python_check_esp(solar_os_agent_set_api_key(python_agent_string(value)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_agent_set_api_key_obj, solaros_agent_set_api_key);

static mp_obj_t solaros_agent_set_reasoning_effort(mp_obj_t value)
{
    python_check_esp(solar_os_agent_set_reasoning_effort(python_agent_string(value)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_agent_set_reasoning_effort_obj, solaros_agent_set_reasoning_effort);

static mp_obj_t solaros_agent_set_tool_policy(mp_obj_t value)
{
    solar_os_agent_tool_policy_t policy;
    python_check_esp(solar_os_agent_parse_tool_policy(python_agent_string(value), &policy));
    python_check_esp(solar_os_agent_set_tool_policy(policy));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_agent_set_tool_policy_obj, solaros_agent_set_tool_policy);

static mp_obj_t solaros_agent_set_max_tools(mp_obj_t value)
{
    const mp_int_t limit = mp_obj_get_int(value);
    if (limit < (mp_int_t)SOLAR_OS_AGENT_MAX_TOOLS_MIN || limit > (mp_int_t)SOLAR_OS_AGENT_MAX_TOOLS_MAX)
        mp_raise_ValueError(MP_ERROR_TEXT("agent max_tools must be 1..32"));
    python_check_esp(solar_os_agent_set_max_tools((uint8_t)limit));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_agent_set_max_tools_obj, solaros_agent_set_max_tools);

static mp_obj_t solaros_agent_cancel(void)
{
    python_check_esp(solar_os_agent_cancel());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_agent_cancel_obj, solaros_agent_cancel);

static mp_obj_t solaros_agent_forget(void)
{
    python_check_esp(solar_os_agent_forget());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_agent_forget_obj, solaros_agent_forget);

static mp_obj_t python_agent_conversation_info(const solar_os_agent_conversation_info_t *info)
{
    mp_obj_t result = mp_obj_new_dict(8);
    python_dict_store_cstr(result, "id", info->id);
    python_dict_store_cstr(result, "title", info->title);
    python_dict_store_cstr(result, "provider", info->provider);
    python_dict_store_cstr(result, "model", info->model);
    python_dict_store_uint(result, "turn_count", info->turn_count);
    python_dict_store_uint(result, "stored_bytes", info->stored_bytes);
    python_dict_store_u64(result, "created_at", info->created_at);
    python_dict_store_u64(result, "updated_at", info->updated_at);
    return result;
}

static mp_obj_t solaros_agent_conversations(void)
{
    solar_os_agent_conversation_info_t items[SOLAR_OS_AGENT_CONVERSATION_LIST_MAX];
    size_t count;
    python_check_esp(solar_os_agent_conversations_list(items, SOLAR_OS_AGENT_CONVERSATION_LIST_MAX, &count));
    mp_obj_t result = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < count; i++) mp_obj_list_append(result, python_agent_conversation_info(&items[i]));
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_agent_conversations_obj, solaros_agent_conversations);

static mp_obj_t solaros_agent_conversation_info(mp_obj_t id)
{
    solar_os_agent_conversation_info_t info;
    python_check_esp(solar_os_agent_conversation_get(python_agent_string(id), &info));
    return python_agent_conversation_info(&info);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_agent_conversation_info_obj, solaros_agent_conversation_info);

static mp_obj_t solaros_agent_delete_conversation(mp_obj_t id)
{
    python_check_esp(solar_os_agent_conversation_delete(python_agent_string(id)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_agent_delete_conversation_obj, solaros_agent_delete_conversation);
#endif
