#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "esp_err.h"
#include "solar_os_memory.h"
#include "solar_os_work.h"

#define HTTPD_JOB_CHUNK_SIZE 1024
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
typedef int httpd_req_t;
static unsigned checkpoints, allocations, calls, fail_send;
static char received[3000];
static size_t received_size;
void solar_os_work_begin(solar_os_work_budget_t *budget) { budget->last_yield_us = 0; }
void solar_os_work_checkpoint(solar_os_work_budget_t *budget) { (void)budget; checkpoints++; }
void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; allocations++; return malloc(bytes); }
void solar_os_memory_free(void *p) { if (p) allocations--; free(p); }
static const char *content_type_for_path(const char *path) { (void)path; return "text/plain"; }
static esp_err_t httpd_resp_send_404(httpd_req_t *req) { (void)req; return ESP_ERR_NOT_FOUND; }
static esp_err_t httpd_resp_send_err(httpd_req_t *req, int code, const char *text)
{ (void)req; (void)code; (void)text; return ESP_ERR_NO_MEM; }
static esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type)
{ (void)req; (void)type; return ESP_OK; }
static esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *data, size_t length)
{
    (void)req;
    if (++calls == fail_send) return ESP_FAIL;
    assert(received_size + length <= sizeof(received));
    if (length) memcpy(received + received_size, data, length);
    received_size += length;
    return ESP_OK;
}
#include "httpd_send_file.inc"

int main(void)
{
    char name[] = "/tmp/solaros-httpd-work-XXXXXX";
    int fd = mkstemp(name);
    assert(fd >= 0);
    char source[2500];
    for (size_t i = 0; i < sizeof(source); ++i) source[i] = (char)(i % 113);
    assert(write(fd, source, sizeof(source)) == sizeof(source));
    close(fd);
    assert(send_file(NULL, name) == ESP_OK);
    assert(received_size == sizeof(source) && !memcmp(received, source, sizeof(source)));
    assert(checkpoints >= 3 && !allocations);
    checkpoints = calls = received_size = 0;
    fail_send = 2;
    assert(send_file(NULL, name) == ESP_FAIL);
    assert(calls == 2 && checkpoints >= 2 && !allocations);
    unlink(name);
    assert(send_file(NULL, name) == ESP_ERR_NOT_FOUND && !allocations);
    puts("HTTP work checkpoints: full payload, send failure and cleanup passed");
}
