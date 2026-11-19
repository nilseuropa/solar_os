#include "script_agent_test_support.inc"

int main(int argc, char **argv)
{
    assert(argc == 2 && !solar_os_json_init());
    body_output = fopen(argv[1], "w"); assert(body_output);
    solar_os_script_agent_request_t request = {.prompt = "Describe both pictures.",
        .images = {"photo.jpg", "stream:camera0"}, .image_count = 2,
        .should_cancel = script_should_cancel};
    solar_os_script_agent_result_t result;
    for (int responses = 0; responses < 2; responses++) {
        test_endpoint = responses ? "https://test/responses" : "https://test/chat/completions";
        assert(!solar_os_script_agent_ask(&request, &result));
        assert(!strcmp(result.text, "image seen") && !result.truncated && !result.conversation_id[0]);
        assert(result.prompt_tokens == 30 && result.completion_tokens == 3 && result.total_tokens == 33);
        solar_os_script_agent_result_clear(&result); assert(!allocations);
    }
    fclose(body_output); body_output = NULL;
    for (const char **source = (const char *[]){"rtsp://test/media", "pipeline:1", NULL}; *source; source++) {
        request.images[0] = *source; request.image_count = 1;
        assert(!solar_os_script_agent_ask(&request, &result));
        solar_os_script_agent_result_clear(&result); assert(!allocations);
    }
    request.image_count = 0; request.conversation_id = "";
    assert(!solar_os_script_agent_ask(&request, &result) && !strcmp(result.conversation_id, "c1"));
    solar_os_script_agent_result_clear(&result); request.conversation_id = "c1";
    assert(!solar_os_script_agent_ask(&request, &result) && !strcmp(result.conversation_id, "c1"));
    solar_os_script_agent_result_clear(&result);
    long_answer = true;
    assert(!solar_os_script_agent_ask(&request, &result));
    assert(result.truncated && result.text_len == SOLAR_OS_SCRIPT_AGENT_TEXT_MAX);
    assert(strlen(result.text) == result.text_len);
    solar_os_script_agent_result_clear(&result); long_answer = false;

    request.image_count = 3;
    assert(solar_os_script_agent_ask(&request, &result) == ESP_ERR_INVALID_ARG && !allocations);
    request.image_count = 2; request.images[0] = "large.jpg"; request.images[1] = "photo.jpg";
    assert(solar_os_script_agent_ask(&request, &result) == ESP_ERR_INVALID_SIZE && !allocations);
    request.images[0] = "photo.jpg";
    request.images[1] = "missing.jpg";
    int runs = agent_runs;
    assert(solar_os_script_agent_ask(&request, &result) == ESP_ERR_NOT_FOUND && !allocations && agent_runs == runs);
    request.images[1] = "unsupported.jpg";
    assert(solar_os_script_agent_ask(&request, &result) == ESP_ERR_NOT_SUPPORTED && !allocations);
    request.images[1] = "stream:camera0";
    script_cancel = true;
    int reads = source_reads;
    assert(solar_os_script_agent_ask(&request, &result) == ESP_ERR_INVALID_STATE && !allocations && source_reads == reads);
    script_cancel = false; cancel_during_source = true; request.image_count = 1;
    assert(solar_os_script_agent_ask(&request, &result) == ESP_ERR_INVALID_STATE && !allocations);
    script_cancel = cancel_during_source = false; cancel_during_run = true;
    assert(solar_os_script_agent_ask(&request, &result) == ESP_ERR_INVALID_STATE && !allocations);
    script_cancel = cancel_during_run = false; agent_error = ESP_ERR_INVALID_RESPONSE;
    assert(solar_os_script_agent_ask(&request, &result) == agent_error && !allocations);
    agent_error = ESP_OK;
    for (int i = 0; i < 12; i++) {
        fail_after = i;
        esp_err_t error = solar_os_script_agent_ask(&request, &result);
        /* JSON parser allocation failures are surfaced by the provider as an
         * invalid response; either failure must release every native buffer. */
        assert(error == ESP_OK || error == ESP_ERR_NO_MEM || error == ESP_ERR_INVALID_RESPONSE);
        solar_os_script_agent_result_clear(&result); assert(!allocations);
    }
    fail_after = -1;
    puts("Script agent protocols, images, history, limits, cancellation, errors and OOM passed");
}
