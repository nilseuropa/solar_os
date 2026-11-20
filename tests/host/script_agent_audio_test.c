#include "script_agent_test_support.inc"

static bool cancel_read(void *user)
{
    unsigned *polls = user;
    return ++*polls >= 3;
}

int main(int argc, char **argv)
{
    assert(argc == 2 && !solar_os_json_init());
    char paths[2][160];
    script_agent_audio_fixture(paths[0], false);
    script_agent_audio_fixture(paths[1], true);
    body_output = fopen(argv[1], "w"); assert(body_output);
    test_endpoint = "https://api.openai.com/v1/chat/completions";
    solar_os_script_agent_request_t request = {.prompt = "Summarize this recording.",
        .should_cancel = script_should_cancel};
    solar_os_script_agent_result_t result;
    for (unsigned i = 0; i < 2; i++) {
        request.audio_file = paths[i];
        assert(!solar_os_script_agent_ask(&request, &result));
        assert(!strcmp(result.text, "image seen") && result.total_tokens == 33);
        solar_os_script_agent_result_clear(&result); assert(!allocations);
    }
    /* A tool continuation must keep the original audio in its user message. */
    solar_os_media_sample_t *sample;
    assert(!solar_os_media_sample_open_audio_bounded(paths[0], SOLAR_OS_AGENT_AUDIO_BYTES_MAX, NULL, NULL, &sample));
    assert(!solar_os_media_sample_image(sample));
    assert(!strcmp(solar_os_media_sample_audio(sample)->format, "wav"));
    solar_os_agent_provider_config_t config = {0};
    strcpy(config.endpoint, test_endpoint); strcpy(config.model, "audio-test");
    solar_os_agent_provider_turn_t turn = {.prompt = request.prompt, .audio = sample,
        .continuation = true, .tool_call_id = "call1", .tool_name = "system_status",
        .tool_arguments = "{}", .tool_result = "{}", .tool_context = "earlier result"};
    record(body_output, &config, &turn);
    solar_os_media_sample_release(sample); assert(!allocations);

    /* The maximum attachment still fits the dynamic base64 request body. */
    uint8_t *large = calloc(1, SOLAR_OS_AGENT_AUDIO_BYTES_MAX + 1); assert(large);
    memcpy(large, script_agent_test_wav, sizeof(script_agent_test_wav));
    assert(!solar_os_media_sample_copy_audio(large, SOLAR_OS_AGENT_AUDIO_BYTES_MAX, NULL, &sample));
    turn.continuation = false; turn.audio = sample;
    record(body_output, &config, &turn);
    solar_os_media_sample_release(sample);
    assert(!solar_os_media_sample_copy_audio(large, SOLAR_OS_AGENT_AUDIO_BYTES_MAX + 1, NULL, &sample));
    assert(solar_os_agent_audio_validate(sample) == ESP_ERR_INVALID_SIZE);
    solar_os_media_sample_release(sample); free(large); assert(!allocations);
    strcpy(config.model, "gpt-audio-1.5");
    turn.audio = NULL; turn.prompt = "Summarize your previous answer.";
    record(body_output, &config, &turn);
    fclose(body_output); body_output = NULL;

    request.audio_file = paths[0]; request.conversation_id = "";
    assert(!solar_os_script_agent_ask(&request, &result) && !strcmp(result.conversation_id, "c1"));
    solar_os_script_agent_result_clear(&result);
    request.conversation_id = "c1";
    assert(!solar_os_script_agent_ask(&request, &result));
    solar_os_script_agent_result_clear(&result);
    int before = requests;
    test_endpoint = "https://test/responses";
    assert(solar_os_script_agent_ask(&request, &result) == ESP_ERR_NOT_SUPPORTED && !allocations && requests == before);
    test_endpoint = "https://api.openai.com/v1/chat/completions";
    request.audio_file = "/tmp/solaros-no-such-audio-file.wav";
    assert(solar_os_script_agent_ask(&request, &result) == ESP_ERR_NOT_FOUND && !allocations);
    request.audio_file = paths[0];
    unsigned polls = 0;
    request.should_cancel = cancel_read; request.user_data = &polls;
    assert(solar_os_script_agent_ask(&request, &result) == ESP_ERR_INVALID_STATE && !allocations && polls >= 3);
    request.should_cancel = script_should_cancel; request.user_data = NULL;
    agent_error = ESP_ERR_INVALID_RESPONSE;
    assert(solar_os_script_agent_ask(&request, &result) == agent_error && !allocations);
    agent_error = ESP_OK;
    for (int i = 0; i < 16; i++) {
        fail_after = i;
        esp_err_t error = solar_os_script_agent_ask(&request, &result);
        assert(error == ESP_OK || error == ESP_ERR_NO_MEM || error == ESP_ERR_INVALID_RESPONSE);
        solar_os_script_agent_result_clear(&result); assert(!allocations);
    }
    fail_after = -1;
    FILE *file = fopen(paths[0], "wb"); assert(file); assert(!fclose(file));
    assert(solar_os_script_agent_ask(&request, &result) == ESP_ERR_INVALID_SIZE && !allocations);
    file = fopen(paths[0], "wb"); assert(file); fputs("not audio", file); assert(!fclose(file));
    assert(solar_os_script_agent_ask(&request, &result) == ESP_ERR_NOT_SUPPORTED && !allocations);
    file = fopen(paths[0], "wb"); assert(file); assert(!ftruncate(fileno(file), SOLAR_OS_AGENT_AUDIO_BYTES_MAX + 1)); assert(!fclose(file));
    assert(solar_os_script_agent_ask(&request, &result) == ESP_ERR_INVALID_SIZE && !allocations);
    uint8_t jpeg[] = {255, 216, 255};
    assert(!solar_os_media_sample_copy_image(jpeg, sizeof(jpeg), NULL, &sample));
    assert(!solar_os_media_sample_audio(sample) && solar_os_agent_audio_validate(sample) == ESP_ERR_NOT_SUPPORTED);
    solar_os_media_sample_release(sample);
    for (unsigned i = 0; i < 2; i++) assert(!unlink(paths[i]));
    assert(!allocations);
    puts("Audio files, WAV/MP3 payloads, continuations, limits, cancellation, errors and OOM passed");
}
