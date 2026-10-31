/* Per-domain python conversions; shared service APIs own policy and state. */
#if SOLAR_OS_PACKAGE_SERVICE_HTTP_CLIENT
static solar_os_http_stream_session_t *python_http_stream_get(void)
{
    if (python_http_stream_session == NULL) {
        python_check_esp(solar_os_http_stream_session_create(
            python_should_cancel,
            NULL,
            &python_http_stream_session));
    }
    return python_http_stream_session;
}

static void python_http_stream_destroy(void)
{
    solar_os_http_stream_session_destroy(python_http_stream_session);
    python_http_stream_session = NULL;
}

static solar_os_http_session_context_t *python_http_session_get(void)
{
    if (python_http_session_context == NULL) {
        python_check_esp(solar_os_http_session_context_create(
            python_should_cancel,
            NULL,
            &python_http_session_context));
    }
    return python_http_session_context;
}

static void python_http_session_destroy(void)
{
    solar_os_http_session_context_destroy(python_http_session_context);
    python_http_session_context = NULL;
}

static solar_os_http_header_t *python_http_headers_from_obj(mp_obj_t headers_obj,
                                                            size_t *header_count)
{
    *header_count = 0;
    if (headers_obj == mp_const_none) {
        return NULL;
    }
    if (!mp_obj_is_dict_or_ordereddict(headers_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected headers dict"));
    }

    mp_map_t *map = mp_obj_dict_get_map(headers_obj);
    if (map->used > PYTHON_HTTP_MAX_REQUEST_HEADERS) {
        mp_raise_ValueError(MP_ERROR_TEXT("too many HTTP headers"));
    }
    if (map->used == 0) {
        return NULL;
    }

    solar_os_http_header_t *headers = solar_os_memory_calloc(
        map->used,
        sizeof(*headers),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "python.http.headers");
    if (headers == NULL) {
        python_raise_esp(ESP_ERR_NO_MEM);
    }

    size_t header_bytes = 0;
    for (size_t i = 0; i < map->alloc; i++) {
        if (!mp_map_slot_is_filled(map, i)) {
            continue;
        }
        if (!mp_obj_is_str(map->table[i].key) ||
            !mp_obj_is_str(map->table[i].value)) {
            solar_os_memory_free(headers);
            mp_raise_TypeError(MP_ERROR_TEXT("HTTP header names and values must be strings"));
        }
        size_t name_len = 0;
        size_t value_len = 0;
        const char *name = mp_obj_str_get_data(map->table[i].key, &name_len);
        const char *value = mp_obj_str_get_data(map->table[i].value, &value_len);
        if (name_len == 0 || strlen(name) != name_len || strlen(value) != value_len ||
            strpbrk(name, "\r\n:") != NULL || strpbrk(value, "\r\n") != NULL) {
            solar_os_memory_free(headers);
            mp_raise_ValueError(MP_ERROR_TEXT("invalid HTTP header"));
        }
        if (name_len + value_len + 2U >
            SOLAR_OS_HTTP_BUFFERED_MAX_HEADER_BYTES - header_bytes) {
            solar_os_memory_free(headers);
            mp_raise_ValueError(MP_ERROR_TEXT("HTTP headers exceed 8192 bytes"));
        }
        headers[*header_count].name = name;
        headers[*header_count].value = value;
        header_bytes += name_len + value_len + 2U;
        (*header_count)++;
    }
    return headers;
}

static mp_obj_t python_http_response_to_dict(
    const solar_os_http_buffered_response_t *response)
{
    mp_obj_t headers = mp_obj_new_dict(response->header_count);
    for (size_t i = 0; i < response->header_count; i++) {
        mp_obj_dict_store(
            headers,
            mp_obj_new_str_from_cstr(response->headers[i].name),
            mp_obj_new_str_from_cstr(response->headers[i].value));
    }

    mp_obj_t result = mp_obj_new_dict(8);
    python_dict_store_int(result, "status_code", response->response.status_code);
    python_dict_store_i64(result,
                          "content_length",
                          response->response.content_length);
    python_dict_store_u64(result, "bytes_received", response->response.bytes_received);
    python_dict_store_uint(result, "duration_ms", response->response.duration_ms);
    python_dict_store_bool(result, "truncated", response->body_truncated);
    python_dict_store_bool(result, "headers_truncated", response->headers_truncated);
    mp_obj_dict_store(result, python_key("headers"), headers);
    mp_obj_dict_store(
        result,
        python_key("body"),
        mp_obj_new_bytes(response->body != NULL ? response->body : (const uint8_t *)"",
                         response->body_len));
    return result;
}

static mp_obj_t python_http_perform(solar_os_http_method_t method,
                                    mp_obj_t url_obj,
                                    mp_obj_t body_obj,
                                    mp_obj_t headers_obj,
                                    uint32_t timeout_ms,
                                    size_t max_bytes,
                                    bool follow_redirects)
{
    if (max_bytes > PYTHON_HTTP_MAX_BODY) {
        mp_raise_ValueError(MP_ERROR_TEXT("HTTP max_bytes exceeds 262144"));
    }

    mp_buffer_info_t body = {0};
    if (body_obj != mp_const_none) {
        mp_get_buffer_raise(body_obj, &body, MP_BUFFER_READ);
    }
    size_t url_len = 0;
    const char *url = mp_obj_str_get_data(url_obj, &url_len);
    if (strlen(url) != url_len ||
        (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected http:// or https:// URL"));
    }

    size_t header_count = 0;
    solar_os_http_header_t *headers =
        python_http_headers_from_obj(headers_obj, &header_count);
    const solar_os_http_request_options_t options = {
        .url = url,
        .method = method,
        .headers = headers,
        .header_count = header_count,
        .body = body.buf,
        .body_len = body.len,
        .user_agent = "SolarOS/" SOLAR_OS_VERSION " script",
        .follow_redirects = follow_redirects,
        .timeout_ms = timeout_ms,
        .read_poll_ms = PYTHON_HTTP_READ_POLL_MS,
        .deadline_ms = timeout_ms,
        .should_cancel = python_should_cancel,
    };
    solar_os_http_buffered_response_t response = {0};
    const esp_err_t err = solar_os_http_perform_buffered(&options,
                                                         method == SOLAR_OS_HTTP_METHOD_HEAD ?
                                                             0U : max_bytes,
                                                         &response);
    solar_os_memory_free(headers);
    if (err != ESP_OK) {
        solar_os_http_buffered_response_clear(&response);
        python_raise_esp(err);
    }

    mp_obj_t result = python_http_response_to_dict(&response);
    solar_os_http_buffered_response_clear(&response);
    return result;
}

static mp_obj_t solaros_http_session_open(mp_obj_t origin_obj)
{
    size_t origin_len = 0;
    const char *origin = mp_obj_str_get_data(origin_obj, &origin_len);
    if (strlen(origin) != origin_len) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid HTTP origin"));
    }
    uint32_t handle = 0;
    python_check_esp(solar_os_http_session_open(
        python_http_session_get(),
        origin,
        "SolarOS/" SOLAR_OS_VERSION " script",
        &handle));
    return mp_obj_new_int_from_uint(handle);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_http_session_open_obj,
                          solaros_http_session_open);

static mp_obj_t solaros_http_session_request(size_t n_args,
                                             const mp_obj_t *args)
{
    solar_os_http_method_t method;
    if (!solar_os_http_method_parse(mp_obj_str_get_str(args[1]), &method)) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected GET, POST, PUT, PATCH, DELETE, or HEAD"));
    }
    const size_t max_bytes = python_optional_u32(
        n_args,
        args,
        6,
        SOLAR_OS_HTTP_BUFFERED_DEFAULT_MAX_BODY);
    if (max_bytes > PYTHON_HTTP_MAX_BODY) {
        mp_raise_ValueError(MP_ERROR_TEXT("HTTP max_bytes exceeds 262144"));
    }

    size_t url_len = 0;
    const char *url = mp_obj_str_get_data(args[2], &url_len);
    if (strlen(url) != url_len ||
        (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected http:// or https:// URL"));
    }
    mp_buffer_info_t body = {0};
    if (n_args >= 4 && args[3] != mp_const_none) {
        mp_get_buffer_raise(args[3], &body, MP_BUFFER_READ);
    }
    size_t header_count = 0;
    solar_os_http_header_t *headers = python_http_headers_from_obj(
        n_args >= 5 ? args[4] : mp_const_none,
        &header_count);
    const uint32_t timeout_ms = python_optional_u32(
        n_args,
        args,
        5,
        PYTHON_HTTP_DEFAULT_TIMEOUT_MS);
    const solar_os_http_request_options_t options = {
        .url = url,
        .method = method,
        .headers = headers,
        .header_count = header_count,
        .body = body.buf,
        .body_len = body.len,
        .timeout_ms = timeout_ms,
        .deadline_ms = timeout_ms,
    };
    solar_os_http_buffered_response_t response = {0};
    const esp_err_t err = solar_os_http_session_request(
        python_http_session_get(),
        python_u32_from_obj(args[0]),
        &options,
        method == SOLAR_OS_HTTP_METHOD_HEAD ? 0U : max_bytes,
        &response);
    solar_os_memory_free(headers);
    if (err != ESP_OK) {
        solar_os_http_buffered_response_clear(&response);
        python_raise_esp(err);
    }
    mp_obj_t result = python_http_response_to_dict(&response);
    solar_os_http_buffered_response_clear(&response);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_http_session_request_obj,
                                    3,
                                    7,
                                    solaros_http_session_request);

static mp_obj_t solaros_http_session_close(mp_obj_t handle_obj)
{
    if (python_http_session_context != NULL) {
        const esp_err_t err = solar_os_http_session_close(
            python_http_session_context,
            python_u32_from_obj(handle_obj));
        if (err != ESP_ERR_NOT_FOUND) {
            python_check_esp(err);
        }
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_http_session_close_obj,
                          solaros_http_session_close);

static mp_obj_t solaros_http_session_close_all(void)
{
    if (python_http_session_context != NULL) {
        solar_os_http_session_close_all(python_http_session_context);
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_http_session_close_all_obj,
                          solaros_http_session_close_all);

static mp_obj_t solaros_http_request(size_t n_args, const mp_obj_t *args)
{
    solar_os_http_method_t method;
    if (!solar_os_http_method_parse(mp_obj_str_get_str(args[0]), &method)) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected GET, POST, PUT, PATCH, DELETE, or HEAD"));
    }
    return python_http_perform(
        method,
        args[1],
        n_args >= 3 ? args[2] : mp_const_none,
        n_args >= 4 ? args[3] : mp_const_none,
        python_optional_u32(n_args, args, 4, PYTHON_HTTP_DEFAULT_TIMEOUT_MS),
        python_optional_u32(n_args,
                            args,
                            5,
                            SOLAR_OS_HTTP_BUFFERED_DEFAULT_MAX_BODY),
        n_args < 7 || args[6] == mp_const_none || mp_obj_is_true(args[6]));
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_http_request_obj, 2, 7, solaros_http_request);

static mp_obj_t python_http_get_head(solar_os_http_method_t method,
                                     size_t n_args,
                                     const mp_obj_t *args)
{
    return python_http_perform(
        method,
        args[0],
        mp_const_none,
        n_args >= 2 ? args[1] : mp_const_none,
        python_optional_u32(n_args, args, 2, PYTHON_HTTP_DEFAULT_TIMEOUT_MS),
        python_optional_u32(n_args,
                            args,
                            3,
                            SOLAR_OS_HTTP_BUFFERED_DEFAULT_MAX_BODY),
        n_args < 5 || args[4] == mp_const_none || mp_obj_is_true(args[4]));
}

static mp_obj_t solaros_http_get(size_t n_args, const mp_obj_t *args)
{
    return python_http_get_head(SOLAR_OS_HTTP_METHOD_GET, n_args, args);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_http_get_obj, 1, 5, solaros_http_get);

static mp_obj_t solaros_http_head(size_t n_args, const mp_obj_t *args)
{
    return python_http_get_head(SOLAR_OS_HTTP_METHOD_HEAD, n_args, args);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_http_head_obj, 1, 5, solaros_http_head);

static mp_obj_t python_http_with_body(solar_os_http_method_t method,
                                      size_t n_args,
                                      const mp_obj_t *args)
{
    return python_http_perform(
        method,
        args[0],
        n_args >= 2 ? args[1] : mp_const_none,
        n_args >= 3 ? args[2] : mp_const_none,
        python_optional_u32(n_args, args, 3, PYTHON_HTTP_DEFAULT_TIMEOUT_MS),
        python_optional_u32(n_args,
                            args,
                            4,
                            SOLAR_OS_HTTP_BUFFERED_DEFAULT_MAX_BODY),
        n_args < 6 || args[5] == mp_const_none || mp_obj_is_true(args[5]));
}

#define PYTHON_HTTP_BODY_METHOD(name, method) \
    static mp_obj_t solaros_http_##name(size_t n_args, const mp_obj_t *args) \
    { \
        return python_http_with_body(method, n_args, args); \
    } \
    MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( \
        solaros_http_##name##_obj, 1, 6, solaros_http_##name)

PYTHON_HTTP_BODY_METHOD(post, SOLAR_OS_HTTP_METHOD_POST);
PYTHON_HTTP_BODY_METHOD(put, SOLAR_OS_HTTP_METHOD_PUT);
PYTHON_HTTP_BODY_METHOD(patch, SOLAR_OS_HTTP_METHOD_PATCH);
PYTHON_HTTP_BODY_METHOD(delete, SOLAR_OS_HTTP_METHOD_DELETE);
#undef PYTHON_HTTP_BODY_METHOD

static mp_obj_t solaros_http_stream_open(size_t n_args, const mp_obj_t *args)
{
    solar_os_http_method_t method;
    if (!solar_os_http_method_parse(mp_obj_str_get_str(args[0]), &method)) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected GET, POST, PUT, PATCH, DELETE, or HEAD"));
    }

    size_t url_len = 0;
    const char *url = mp_obj_str_get_data(args[1], &url_len);
    if (strlen(url) != url_len ||
        (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected http:// or https:// URL"));
    }
    mp_buffer_info_t body = {0};
    if (n_args >= 3 && args[2] != mp_const_none) {
        mp_get_buffer_raise(args[2], &body, MP_BUFFER_READ);
    }
    size_t header_count = 0;
    solar_os_http_header_t *headers = python_http_headers_from_obj(
        n_args >= 4 ? args[3] : mp_const_none,
        &header_count);
    const uint32_t timeout_ms = python_optional_u32(
        n_args,
        args,
        4,
        PYTHON_HTTP_DEFAULT_TIMEOUT_MS);
    const solar_os_http_request_options_t options = {
        .url = url,
        .method = method,
        .headers = headers,
        .header_count = header_count,
        .body = body.buf,
        .body_len = body.len,
        .user_agent = "SolarOS/" SOLAR_OS_VERSION " script",
        .follow_redirects = n_args < 6 || args[5] == mp_const_none ||
            mp_obj_is_true(args[5]),
        .timeout_ms = timeout_ms,
        .read_poll_ms = PYTHON_HTTP_READ_POLL_MS,
    };
    uint32_t handle = 0;
    const esp_err_t err = solar_os_http_stream_open(python_http_stream_get(),
                                                    &options,
                                                    &handle);
    solar_os_memory_free(headers);
    python_check_esp(err);
    return mp_obj_new_int_from_uint(handle);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_http_stream_open_obj,
                                    2,
                                    6,
                                    solaros_http_stream_open);

static mp_obj_t python_http_stream_event_to_dict(
    const solar_os_http_stream_event_t *event)
{
    mp_obj_t result = mp_obj_new_dict(12);
    python_dict_store_cstr(result,
                           "type",
                           solar_os_http_stream_event_type_name(event->type));
    python_dict_store_int(result, "status_code", event->status_code);
    if (event->type == SOLAR_OS_HTTP_STREAM_EVENT_RESPONSE) {
        python_dict_store_i64(result, "content_length", event->content_length);
    } else if (event->type == SOLAR_OS_HTTP_STREAM_EVENT_HEADER) {
        python_dict_store_cstr(result, "name", event->header_name);
        python_dict_store_cstr(result, "value", event->header_value);
        python_dict_store_bool(result, "truncated", event->truncated);
    } else if (event->type == SOLAR_OS_HTTP_STREAM_EVENT_DATA) {
        mp_obj_dict_store(result,
                          python_key("data"),
                          mp_obj_new_bytes(event->data, event->data_len));
    } else {
        python_dict_store_i64(result, "content_length", event->content_length);
        python_dict_store_u64(result, "bytes_received", event->bytes_received);
        python_dict_store_uint(result, "duration_ms", event->duration_ms);
        python_dict_store_int(result, "error", event->error);
        python_dict_store_cstr(result, "error_name", esp_err_to_name(event->error));
        python_dict_store_bool(result, "cancelled", event->cancelled);
        python_dict_store_bool(result,
                               "deadline_exceeded",
                               event->deadline_exceeded);
    }
    return result;
}

static mp_obj_t solaros_http_stream_read(size_t n_args, const mp_obj_t *args)
{
    if (python_http_stream_session == NULL) {
        return mp_const_none;
    }
    solar_os_http_stream_event_t event;
    const esp_err_t err = solar_os_http_stream_read(
        python_http_stream_session,
        python_u32_from_obj(args[0]),
        python_optional_u32(n_args, args, 1, 0),
        &event);
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_STATE) {
        return mp_const_none;
    }
    python_check_esp(err);
    return python_http_stream_event_to_dict(&event);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_http_stream_read_obj,
                                    1,
                                    2,
                                    solaros_http_stream_read);

static mp_obj_t solaros_http_stream_close(mp_obj_t handle_obj)
{
    if (python_http_stream_session != NULL) {
        const esp_err_t err = solar_os_http_stream_close(
            python_http_stream_session,
            python_u32_from_obj(handle_obj));
        if (err != ESP_ERR_NOT_FOUND) {
            python_check_esp(err);
        }
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_http_stream_close_obj,
                          solaros_http_stream_close);

static mp_obj_t solaros_http_stream_close_all(void)
{
    if (python_http_stream_session != NULL) {
        solar_os_http_stream_close_all(python_http_stream_session);
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_http_stream_close_all_obj,
                          solaros_http_stream_close_all);
#endif

static int python_gpio_pin_from_obj(mp_obj_t obj)
{
    const mp_int_t pin = mp_obj_get_int(obj);
    if (pin < 0 || pin > 48) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected GPIO pin 0..48"));
    }
    return (int)pin;
}
