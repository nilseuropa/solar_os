#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "esp_err.h"

typedef void *httpd_handle_t;

typedef enum http_method {
    HTTP_GET,
    HTTP_POST,
} httpd_method_t;

typedef struct httpd_req {
    const char *uri;
    httpd_method_t method;
    void *user_ctx;
    size_t content_len, body_offset;
} httpd_req_t;

typedef esp_err_t (*httpd_uri_handler_t)(httpd_req_t *req);
typedef bool (*httpd_uri_match_func_t)(const char *template,
                                      const char *uri,
                                      size_t len);

typedef struct {
    const char *uri;
    httpd_method_t method;
    httpd_uri_handler_t handler;
    void *user_ctx;
} httpd_uri_t;

typedef struct {
    size_t stack_size;
    size_t max_open_sockets;
    bool lru_purge_enable;
    uint16_t server_port;
    httpd_uri_match_func_t uri_match_fn;
} httpd_config_t;

#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t) {.server_port = 80U})

typedef enum {
    HTTPD_500_INTERNAL_SERVER_ERROR,
    HTTPD_401_UNAUTHORIZED,
} httpd_err_code_t;

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle,
                                     const httpd_uri_t *uri);
bool httpd_uri_match_wildcard(const char *template,
                              const char *uri,
                              size_t len);
size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req,
                                      const char *field,
                                      char *value,
                                      size_t value_len);
esp_err_t httpd_resp_send_404(httpd_req_t *req);
esp_err_t httpd_resp_set_hdr(httpd_req_t *req,
                             const char *field,
                             const char *value);
esp_err_t httpd_resp_send_err(httpd_req_t *req,
                              httpd_err_code_t error,
                              const char *message);
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status);
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *message);
esp_err_t httpd_req_async_handler_begin(httpd_req_t *req,
                                        httpd_req_t **out);
esp_err_t httpd_req_async_handler_complete(httpd_req_t *req);

#define HTTPD_SOCK_ERR_TIMEOUT (-3)
int httpd_req_recv(httpd_req_t *r, char *data, size_t capacity);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *data, ssize_t length);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *data, ssize_t length);
