/* Per-domain lua conversions; shared service APIs own policy and state. */
#if SOLAR_OS_PACKAGE_SERVICE_HTTP_CLIENT
static solar_os_http_stream_session_t *solua_http_stream_get(lua_State *L)
{
    if (solua_http_stream_session == NULL) {
        (void)solua_check_esp(L,
                              solar_os_http_stream_session_create(
                                  solua_should_cancel,
                                  NULL,
                                  &solua_http_stream_session));
    }
    return solua_http_stream_session;
}

static void solua_http_stream_destroy(void)
{
    solar_os_http_stream_session_destroy(solua_http_stream_session);
    solua_http_stream_session = NULL;
}

static solar_os_http_session_context_t *solua_http_session_get(lua_State *L)
{
    if (solua_http_session_context == NULL) {
        (void)solua_check_esp(L,
                              solar_os_http_session_context_create(
                                  solua_should_cancel,
                                  NULL,
                                  &solua_http_session_context));
    }
    return solua_http_session_context;
}

static void solua_http_session_destroy(void)
{
    solar_os_http_session_context_destroy(solua_http_session_context);
    solua_http_session_context = NULL;
}

static solar_os_http_header_t *solua_http_headers_from_table(lua_State *L,
                                                             int index,
                                                             size_t *header_count)
{
    *header_count = 0;
    if (lua_isnoneornil(L, index)) {
        return NULL;
    }
    luaL_checktype(L, index, LUA_TTABLE);
    index = lua_absindex(L, index);

    size_t header_bytes = 0;
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 2);
            luaL_error(L, "HTTP header names and values must be strings");
        }
        size_t name_len = 0;
        size_t value_len = 0;
        const char *name = lua_tolstring(L, -2, &name_len);
        const char *value = lua_tolstring(L, -1, &value_len);
        if (name_len == 0 || strlen(name) != name_len || strlen(value) != value_len ||
            strpbrk(name, "\r\n:") != NULL || strpbrk(value, "\r\n") != NULL) {
            lua_pop(L, 2);
            luaL_error(L, "invalid HTTP header");
        }
        if (name_len + value_len + 2U >
            SOLAR_OS_HTTP_BUFFERED_MAX_HEADER_BYTES - header_bytes) {
            lua_pop(L, 2);
            luaL_error(L, "HTTP headers exceed 8192 bytes");
        }
        header_bytes += name_len + value_len + 2U;
        (*header_count)++;
        lua_pop(L, 1);
    }
    if (*header_count > SOLUA_HTTP_MAX_REQUEST_HEADERS) {
        luaL_error(L, "too many HTTP headers");
    }
    if (*header_count == 0) {
        return NULL;
    }

    solar_os_http_header_t *headers = solar_os_memory_calloc(
        *header_count,
        sizeof(*headers),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "lua.http.headers");
    if (headers == NULL) {
        luaL_error(L, "%s", esp_err_to_name(ESP_ERR_NO_MEM));
    }

    size_t current = 0;
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        headers[current].name = lua_tostring(L, -2);
        headers[current].value = lua_tostring(L, -1);
        current++;
        lua_pop(L, 1);
    }
    return headers;
}

static size_t solua_http_max_bytes(lua_State *L, int index)
{
    if (lua_isnoneornil(L, index)) {
        return SOLAR_OS_HTTP_BUFFERED_DEFAULT_MAX_BODY;
    }
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < 0 || (lua_Unsigned)value > SOLUA_HTTP_MAX_BODY) {
        luaL_error(L, "HTTP max_bytes must be 0..262144");
    }
    return (size_t)value;
}

static uint32_t solua_http_timeout_ms(lua_State *L, int index)
{
    if (lua_isnoneornil(L, index)) {
        return SOLUA_HTTP_DEFAULT_TIMEOUT_MS;
    }
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < 0 || value > INT_MAX) {
        luaL_error(L, "HTTP timeout_ms must be 0..2147483647");
    }
    return (uint32_t)value;
}

static int solua_http_push_response(
    lua_State *L,
    const solar_os_http_buffered_response_t *response)
{
    lua_newtable(L);
    const int result = lua_gettop(L);
    solua_set_int(L, result, "status_code", response->response.status_code);
    solua_set_int(L, result, "content_length", response->response.content_length);
    solua_set_int(L, result, "bytes_received", response->response.bytes_received);
    solua_set_int(L, result, "duration_ms", response->response.duration_ms);
    solua_set_bool(L, result, "truncated", response->body_truncated);
    solua_set_bool(L, result, "headers_truncated", response->headers_truncated);

    lua_newtable(L);
    const int headers = lua_gettop(L);
    for (size_t i = 0; i < response->header_count; i++) {
        lua_pushstring(L, response->headers[i].value);
        lua_setfield(L, headers, response->headers[i].name);
    }
    lua_setfield(L, result, "headers");

    lua_pushlstring(L,
                    response->body != NULL ? (const char *)response->body : "",
                    response->body_len);
    lua_setfield(L, result, "body");
    return 1;
}

static int solua_http_perform(lua_State *L,
                              solar_os_http_method_t method,
                              int url_index,
                              int body_index,
                              int headers_index,
                              int timeout_index,
                              int max_bytes_index,
                              int redirects_index)
{
    size_t url_len = 0;
    const char *url = luaL_checklstring(L, url_index, &url_len);
    if (strlen(url) != url_len ||
        (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        return luaL_error(L, "expected http:// or https:// URL");
    }
    size_t body_len = 0;
    const char *body = lua_isnoneornil(L, body_index) ?
        NULL : luaL_checklstring(L, body_index, &body_len);
    const uint32_t timeout_ms = solua_http_timeout_ms(L, timeout_index);
    const size_t max_bytes = solua_http_max_bytes(L, max_bytes_index);
    const bool follow_redirects = lua_isnoneornil(L, redirects_index) ||
        lua_toboolean(L, redirects_index);
    size_t header_count = 0;
    solar_os_http_header_t *headers =
        solua_http_headers_from_table(L, headers_index, &header_count);
    const solar_os_http_request_options_t options = {
        .url = url,
        .method = method,
        .headers = headers,
        .header_count = header_count,
        .body = body,
        .body_len = body_len,
        .user_agent = "SolarOS/" SOLAR_OS_VERSION " script",
        .follow_redirects = follow_redirects,
        .timeout_ms = timeout_ms,
        .read_poll_ms = SOLUA_HTTP_READ_POLL_MS,
        .deadline_ms = timeout_ms,
        .should_cancel = solua_should_cancel,
    };
    solar_os_http_buffered_response_t response = {0};
    const esp_err_t err = solar_os_http_perform_buffered(&options,
                                                         method == SOLAR_OS_HTTP_METHOD_HEAD ?
                                                             0U : max_bytes,
                                                         &response);
    solar_os_memory_free(headers);
    if (err != ESP_OK) {
        solar_os_http_buffered_response_clear(&response);
        return solua_check_esp(L, err);
    }

    const int result_count = solua_http_push_response(L, &response);
    solar_os_http_buffered_response_clear(&response);
    return result_count;
}

static int solua_http_session_open(lua_State *L)
{
    size_t origin_len = 0;
    const char *origin = luaL_checklstring(L, 1, &origin_len);
    if (strlen(origin) != origin_len) {
        return luaL_error(L, "invalid HTTP origin");
    }
    uint32_t handle = 0;
    (void)solua_check_esp(L,
                          solar_os_http_session_open(
                              solua_http_session_get(L),
                              origin,
                              "SolarOS/" SOLAR_OS_VERSION " script",
                              &handle));
    lua_pushinteger(L, handle);
    return 1;
}

static int solua_http_session_request(lua_State *L)
{
    solar_os_http_method_t method;
    if (!solar_os_http_method_parse(luaL_checkstring(L, 2), &method)) {
        return luaL_error(L, "expected GET, POST, PUT, PATCH, DELETE, or HEAD");
    }
    size_t url_len = 0;
    const char *url = luaL_checklstring(L, 3, &url_len);
    if (strlen(url) != url_len ||
        (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        return luaL_error(L, "expected http:// or https:// URL");
    }
    size_t body_len = 0;
    const char *body = lua_isnoneornil(L, 4) ?
        NULL : luaL_checklstring(L, 4, &body_len);
    size_t header_count = 0;
    solar_os_http_header_t *headers = solua_http_headers_from_table(
        L,
        5,
        &header_count);
    const uint32_t timeout_ms = solua_http_timeout_ms(L, 6);
    const size_t max_bytes = solua_http_max_bytes(L, 7);
    const solar_os_http_request_options_t options = {
        .url = url,
        .method = method,
        .headers = headers,
        .header_count = header_count,
        .body = body,
        .body_len = body_len,
        .timeout_ms = timeout_ms,
        .deadline_ms = timeout_ms,
    };
    solar_os_http_buffered_response_t response = {0};
    const esp_err_t err = solar_os_http_session_request(
        solua_http_session_get(L),
        solua_check_u32(L, 1),
        &options,
        method == SOLAR_OS_HTTP_METHOD_HEAD ? 0U : max_bytes,
        &response);
    solar_os_memory_free(headers);
    if (err != ESP_OK) {
        solar_os_http_buffered_response_clear(&response);
        return solua_check_esp(L, err);
    }
    const int result_count = solua_http_push_response(L, &response);
    solar_os_http_buffered_response_clear(&response);
    return result_count;
}

static int solua_http_session_close(lua_State *L)
{
    if (solua_http_session_context != NULL) {
        const esp_err_t err = solar_os_http_session_close(
            solua_http_session_context,
            solua_check_u32(L, 1));
        if (err != ESP_ERR_NOT_FOUND) {
            (void)solua_check_esp(L, err);
        }
    }
    return 0;
}

static int solua_http_session_close_all(lua_State *L)
{
    (void)L;
    if (solua_http_session_context != NULL) {
        solar_os_http_session_close_all(solua_http_session_context);
    }
    return 0;
}

static int solua_http_request(lua_State *L)
{
    solar_os_http_method_t method;
    if (!solar_os_http_method_parse(luaL_checkstring(L, 1), &method)) {
        return luaL_error(L, "expected GET, POST, PUT, PATCH, DELETE, or HEAD");
    }
    return solua_http_perform(L, method, 2, 3, 4, 5, 6, 7);
}

static int solua_http_get(lua_State *L)
{
    return solua_http_perform(L, SOLAR_OS_HTTP_METHOD_GET, 1, 0, 2, 3, 4, 5);
}

static int solua_http_head(lua_State *L)
{
    return solua_http_perform(L, SOLAR_OS_HTTP_METHOD_HEAD, 1, 0, 2, 3, 4, 5);
}

#define SOLUA_HTTP_BODY_METHOD(name, method) \
    static int solua_http_##name(lua_State *L) \
    { \
        return solua_http_perform(L, method, 1, 2, 3, 4, 5, 6); \
    }

SOLUA_HTTP_BODY_METHOD(post, SOLAR_OS_HTTP_METHOD_POST)
SOLUA_HTTP_BODY_METHOD(put, SOLAR_OS_HTTP_METHOD_PUT)
SOLUA_HTTP_BODY_METHOD(patch, SOLAR_OS_HTTP_METHOD_PATCH)
SOLUA_HTTP_BODY_METHOD(delete, SOLAR_OS_HTTP_METHOD_DELETE)
#undef SOLUA_HTTP_BODY_METHOD

static int solua_http_stream_open(lua_State *L)
{
    solar_os_http_method_t method;
    if (!solar_os_http_method_parse(luaL_checkstring(L, 1), &method)) {
        return luaL_error(L, "expected GET, POST, PUT, PATCH, DELETE, or HEAD");
    }
    size_t url_len = 0;
    const char *url = luaL_checklstring(L, 2, &url_len);
    if (strlen(url) != url_len ||
        (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        return luaL_error(L, "expected http:// or https:// URL");
    }
    size_t body_len = 0;
    const char *body = lua_isnoneornil(L, 3) ?
        NULL : luaL_checklstring(L, 3, &body_len);
    size_t header_count = 0;
    solar_os_http_header_t *headers = solua_http_headers_from_table(
        L,
        4,
        &header_count);
    const solar_os_http_request_options_t options = {
        .url = url,
        .method = method,
        .headers = headers,
        .header_count = header_count,
        .body = body,
        .body_len = body_len,
        .user_agent = "SolarOS/" SOLAR_OS_VERSION " script",
        .follow_redirects = lua_isnoneornil(L, 6) || lua_toboolean(L, 6),
        .timeout_ms = solua_http_timeout_ms(L, 5),
        .read_poll_ms = SOLUA_HTTP_READ_POLL_MS,
    };
    uint32_t handle = 0;
    const esp_err_t err = solar_os_http_stream_open(solua_http_stream_get(L),
                                                    &options,
                                                    &handle);
    solar_os_memory_free(headers);
    (void)solua_check_esp(L, err);
    lua_pushinteger(L, handle);
    return 1;
}

static int solua_http_push_stream_event(
    lua_State *L,
    const solar_os_http_stream_event_t *event)
{
    lua_newtable(L);
    const int result = lua_gettop(L);
    solua_set_str(L,
                  result,
                  "type",
                  solar_os_http_stream_event_type_name(event->type));
    solua_set_int(L, result, "status_code", event->status_code);
    if (event->type == SOLAR_OS_HTTP_STREAM_EVENT_RESPONSE) {
        solua_set_int(L, result, "content_length", event->content_length);
    } else if (event->type == SOLAR_OS_HTTP_STREAM_EVENT_HEADER) {
        solua_set_str(L, result, "name", event->header_name);
        solua_set_str(L, result, "value", event->header_value);
        solua_set_bool(L, result, "truncated", event->truncated);
    } else if (event->type == SOLAR_OS_HTTP_STREAM_EVENT_DATA) {
        lua_pushlstring(L, (const char *)event->data, event->data_len);
        lua_setfield(L, result, "data");
    } else {
        solua_set_int(L, result, "content_length", event->content_length);
        solua_set_int(L, result, "bytes_received", event->bytes_received);
        solua_set_int(L, result, "duration_ms", event->duration_ms);
        solua_set_int(L, result, "error", event->error);
        solua_set_str(L, result, "error_name", esp_err_to_name(event->error));
        solua_set_bool(L, result, "cancelled", event->cancelled);
        solua_set_bool(L,
                       result,
                       "deadline_exceeded",
                       event->deadline_exceeded);
    }
    return 1;
}

static int solua_http_stream_read(lua_State *L)
{
    if (solua_http_stream_session == NULL) {
        lua_pushnil(L);
        return 1;
    }
    solar_os_http_stream_event_t event;
    const esp_err_t err = solar_os_http_stream_read(
        solua_http_stream_session,
        solua_check_u32(L, 1),
        solua_optional_u32(L, 2, 0),
        &event);
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_STATE) {
        lua_pushnil(L);
        return 1;
    }
    (void)solua_check_esp(L, err);
    return solua_http_push_stream_event(L, &event);
}

static int solua_http_stream_close(lua_State *L)
{
    if (solua_http_stream_session != NULL) {
        const esp_err_t err = solar_os_http_stream_close(
            solua_http_stream_session,
            solua_check_u32(L, 1));
        if (err != ESP_ERR_NOT_FOUND) {
            (void)solua_check_esp(L, err);
        }
    }
    return 0;
}

static int solua_http_stream_close_all(lua_State *L)
{
    (void)L;
    if (solua_http_stream_session != NULL) {
        solar_os_http_stream_close_all(solua_http_stream_session);
    }
    return 0;
}
#endif
