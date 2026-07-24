#include "solar_os_email_sync_job.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_email.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_wifi.h"
#include "solar_os_work.h"

#define EMAIL_SYNC_DEFAULT_INTERVAL_SEC 300U
#define EMAIL_SYNC_MIN_INTERVAL_SEC 30U
#define EMAIL_SYNC_MAX_INTERVAL_SEC 86400U
#define EMAIL_SYNC_DEFAULT_PORT 993U
#define EMAIL_SYNC_CONNECT_TIMEOUT_MS 15000U
#define EMAIL_SYNC_IO_TIMEOUT_MS 15000U
#define EMAIL_SYNC_RESPONSE_MAX 8192U
#define EMAIL_SYNC_FETCH_MAX 8U
#define EMAIL_SYNC_HOST_MAX 128U

typedef struct {
    bool running;
    bool once;
    volatile bool sync_in_progress;
    volatile bool stop_requested;
    volatile bool complete_requested;
    uint32_t interval_ms;
    uint32_t next_sync_ms;
    uint32_t last_uid;
    uint32_t uid_validity;
    uint32_t success_count;
    uint32_t fail_count;
    uint32_t generation;
    esp_err_t last_error;
    solar_os_work_handle_t work;
} email_sync_state_t;

typedef struct {
    char host[EMAIL_SYNC_HOST_MAX];
    uint16_t port;
} email_endpoint_t;

static email_sync_state_t email_sync;
static const char *TAG = "email_sync";

static const char *email_sync_find_case(const char *haystack,
                                        size_t haystack_len,
                                        const char *needle)
{
    if (haystack == NULL || needle == NULL) {
        return NULL;
    }
    const size_t needle_len = strlen(needle);
    if (needle_len == 0) {
        return haystack;
    }
    if (needle_len > haystack_len) {
        return NULL;
    }
    for (size_t offset = 0; offset <= haystack_len - needle_len; offset++) {
        if (strncasecmp(haystack + offset, needle, needle_len) == 0) {
            return haystack + offset;
        }
    }
    return NULL;
}

static bool email_sync_parse_interval(const char *text, uint32_t *seconds)
{
    if (text == NULL || seconds == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < EMAIL_SYNC_MIN_INTERVAL_SEC || value > EMAIL_SYNC_MAX_INTERVAL_SEC) {
        return false;
    }
    *seconds = (uint32_t)value;
    return true;
}

static bool email_sync_parse_args(int argc, char **argv, uint32_t *seconds, bool *once)
{
    *seconds = EMAIL_SYNC_DEFAULT_INTERVAL_SEC;
    *once = false;
    int first = 0;
    if (argc > 0 && argv != NULL && argv[0] != NULL &&
        strcmp(argv[0], solar_os_email_sync_job.name) == 0) {
        first = 1;
    }
    bool interval_set = false;
    for (int i = first; i < argc; i++) {
        if (strcmp(argv[i], "once") == 0) {
            if (*once) {
                return false;
            }
            *once = true;
        } else if (!interval_set && email_sync_parse_interval(argv[i], seconds)) {
            interval_set = true;
        } else {
            return false;
        }
    }
    return true;
}

static esp_err_t email_sync_parse_url(const char *url, email_endpoint_t *endpoint)
{
    if (url == NULL || endpoint == NULL || strncmp(url, "imaps://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *p = url + 8;
    const char *host = p;
    while (*p != '\0' && *p != ':') {
        p++;
    }
    const size_t host_len = (size_t)(p - host);
    if (host_len == 0 || host_len >= sizeof(endpoint->host)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(endpoint, 0, sizeof(*endpoint));
    memcpy(endpoint->host, host, host_len);
    endpoint->port = EMAIL_SYNC_DEFAULT_PORT;
    if (*p == ':') {
        p++;
        char *end = NULL;
        errno = 0;
        const unsigned long port = strtoul(p, &end, 10);
        if (errno != 0 || end == p || *end != '\0' || port == 0 || port > UINT16_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        endpoint->port = (uint16_t)port;
    }
    return ESP_OK;
}

static bool email_sync_would_block(ssize_t result)
{
    return result == ESP_TLS_ERR_SSL_WANT_READ ||
        result == ESP_TLS_ERR_SSL_WANT_WRITE ||
        result == ESP_TLS_ERR_SSL_TIMEOUT ||
        (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
}

static esp_err_t email_sync_set_nonblocking(esp_tls_t *tls)
{
    int fd = -1;
    esp_err_t err = esp_tls_get_conn_sockfd(tls, &fd);
    if (err != ESP_OK) {
        return err;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t email_sync_write_all(esp_tls_t *tls, const char *text)
{
    size_t offset = 0;
    const size_t length = strlen(text);
    const int64_t deadline = esp_timer_get_time() + EMAIL_SYNC_IO_TIMEOUT_MS * 1000LL;
    while (offset < length && !email_sync.stop_requested) {
        errno = 0;
        const ssize_t written = esp_tls_conn_write(tls, text + offset, length - offset);
        if (written > 0) {
            offset += (size_t)written;
        } else if (email_sync_would_block(written) && esp_timer_get_time() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            return esp_timer_get_time() >= deadline ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
    }
    return offset == length ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool email_sync_tag_complete(const char *response, const char *tag, bool *ok)
{
    if (response == NULL || tag == NULL) {
        return false;
    }
    const size_t tag_len = strlen(tag);
    const char *line = response;
    while (line != NULL && *line != '\0') {
        if (strncmp(line, tag, tag_len) == 0 && line[tag_len] == ' ') {
            const char *status = line + tag_len + 1U;
            *ok = strncasecmp(status, "OK", 2) == 0;
            return true;
        }
        line = strchr(line, '\n');
        if (line != NULL) {
            line++;
        }
    }
    return false;
}

static esp_err_t email_sync_read(esp_tls_t *tls,
                                 const char *tag,
                                 char *response,
                                 size_t response_len,
                                 bool *response_ok)
{
    size_t used = 0;
    const int64_t deadline = esp_timer_get_time() + EMAIL_SYNC_IO_TIMEOUT_MS * 1000LL;
    if (response_ok != NULL) {
        *response_ok = false;
    }
    response[0] = '\0';
    while (!email_sync.stop_requested && esp_timer_get_time() < deadline) {
        if (used + 1U >= response_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        errno = 0;
        const ssize_t count = esp_tls_conn_read(tls, response + used, response_len - used - 1U);
        if (count > 0) {
            used += (size_t)count;
            response[used] = '\0';
            if (tag == NULL) {
                if (strchr(response, '\n') != NULL) {
                    return strncmp(response, "* OK", 4) == 0 ? ESP_OK : ESP_FAIL;
                }
            } else {
                bool ok = false;
                if (email_sync_tag_complete(response, tag, &ok)) {
                    if (response_ok != NULL) {
                        *response_ok = ok;
                    }
                    return ok ? ESP_OK : ESP_FAIL;
                }
            }
        } else if (count == 0) {
            return ESP_ERR_INVALID_STATE;
        } else if (email_sync_would_block(count)) {
            vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            return ESP_FAIL;
        }
    }
    return email_sync.stop_requested ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
}

static size_t email_sync_quote(const char *text, char *out, size_t out_len)
{
    size_t used = 0;
    if (out_len == 0) {
        return 0;
    }
    for (size_t i = 0; text != NULL && text[i] != '\0'; i++) {
        if ((text[i] == '"' || text[i] == '\\') && used + 1U < out_len) {
            out[used++] = '\\';
        }
        if (used + 1U >= out_len) {
            break;
        }
        out[used++] = text[i];
    }
    out[used] = '\0';
    return used;
}

static esp_err_t email_sync_command(esp_tls_t *tls,
                                    unsigned sequence,
                                    const char *command,
                                    char *response,
                                    size_t response_len)
{
    char tag[8];
    char line[512];
    snprintf(tag, sizeof(tag), "A%03u", sequence);
    const int written = snprintf(line, sizeof(line), "%s %s\r\n", tag, command);
    if (written < 0 || (size_t)written >= sizeof(line)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = email_sync_write_all(tls, line);
    if (err != ESP_OK) {
        return err;
    }
    return email_sync_read(tls, tag, response, response_len, NULL);
}

static uint32_t email_sync_parse_uid_validity(const char *response)
{
    const char *found = response != NULL ?
        email_sync_find_case(response, strlen(response), "UIDVALIDITY ") : NULL;
    if (found == NULL) {
        return 0;
    }
    found += strlen("UIDVALIDITY ");
    return (uint32_t)strtoul(found, NULL, 10);
}

static size_t email_sync_parse_search(const char *response, uint32_t *uids, size_t max_uids)
{
    const char *line = response;
    while (line != NULL && *line != '\0') {
        if (strncasecmp(line, "* SEARCH", 8) == 0) {
            line += 8;
            size_t count = 0;
            while (*line != '\0' && *line != '\r' && *line != '\n') {
                while (*line == ' ') {
                    line++;
                }
                if (!isdigit((unsigned char)*line)) {
                    break;
                }
                char *end = NULL;
                const unsigned long uid = strtoul(line, &end, 10);
                line = end;
                if (uid > 0 && uid <= UINT32_MAX) {
                    if (count < max_uids) {
                        uids[count++] = (uint32_t)uid;
                    }
                }
            }
            return count;
        }
        line = strchr(line, '\n');
        if (line != NULL) {
            line++;
        }
    }
    return 0;
}

static bool email_sync_next_literal(const char *response,
                                    size_t response_len,
                                    size_t *offset,
                                    const char **literal,
                                    size_t *literal_len)
{
    for (size_t i = *offset; i + 4U < response_len; i++) {
        if (response[i] != '{' || !isdigit((unsigned char)response[i + 1U])) {
            continue;
        }
        char *end = NULL;
        const unsigned long length = strtoul(response + i + 1U, &end, 10);
        if (end == NULL || end[0] != '}' || end[1] != '\r' || end[2] != '\n') {
            continue;
        }
        const size_t start = (size_t)(end - response) + 3U;
        if (length > SIZE_MAX - start || start + (size_t)length > response_len) {
            return false;
        }
        *literal = response + start;
        *literal_len = (size_t)length;
        *offset = start + (size_t)length;
        return true;
    }
    return false;
}

static void email_sync_header_value(const char *headers,
                                    size_t headers_len,
                                    const char *name,
                                    char *out,
                                    size_t out_len)
{
    out[0] = '\0';
    const size_t name_len = strlen(name);
    size_t pos = 0;
    while (pos < headers_len) {
        size_t line_end = pos;
        while (line_end < headers_len && headers[line_end] != '\n') {
            line_end++;
        }
        if (line_end - pos > name_len + 1U &&
            strncasecmp(headers + pos, name, name_len) == 0 && headers[pos + name_len] == ':') {
            size_t value = pos + name_len + 1U;
            while (value < line_end && (headers[value] == ' ' || headers[value] == '\t')) {
                value++;
            }
            size_t length = line_end - value;
            while (length > 0 && (headers[value + length - 1U] == '\r' ||
                                  headers[value + length - 1U] == ' ')) {
                length--;
            }
            if (length >= out_len) {
                length = out_len - 1U;
            }
            memcpy(out, headers + value, length);
            out[length] = '\0';
            return;
        }
        pos = line_end < headers_len ? line_end + 1U : headers_len;
    }
}

static void email_sync_clean_preview(const char *body,
                                     size_t body_len,
                                     char *preview,
                                     size_t preview_len,
                                     bool *truncated)
{
    size_t used = 0;
    bool whitespace = false;
    bool in_tag = false;
    for (size_t i = 0; i < body_len && used + 1U < preview_len; i++) {
        const unsigned char ch = (unsigned char)body[i];
        if (ch == '<') {
            in_tag = true;
            whitespace = true;
            continue;
        }
        if (in_tag) {
            if (ch == '>') {
                in_tag = false;
            }
            continue;
        }
        if (isspace(ch)) {
            whitespace = used > 0;
            continue;
        }
        if (whitespace && used + 1U < preview_len) {
            preview[used++] = ' ';
            whitespace = false;
        }
        if (ch >= 0x20U && ch != 0x7fU) {
            preview[used++] = (char)ch;
        }
    }
    preview[used] = '\0';
    if (truncated != NULL && body_len > 0 && used + 1U >= preview_len) {
        *truncated = true;
    }
}

static esp_err_t email_sync_fetch_message(esp_tls_t *tls,
                                          unsigned sequence,
                                          uint32_t uid,
                                          char *response,
                                          size_t response_len)
{
    char command[256];
    snprintf(command,
             sizeof(command),
             "UID FETCH %lu (BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)] BODY.PEEK[TEXT]<0.1024>)",
             (unsigned long)uid);
    esp_err_t err = email_sync_command(tls, sequence, command, response, response_len);
    if (err != ESP_OK) {
        return err;
    }

    const size_t length = strlen(response);
    size_t offset = 0;
    const char *literal = NULL;
    size_t literal_len = 0;
    const char *headers = NULL;
    size_t headers_len = 0;
    const char *body = NULL;
    size_t body_len = 0;
    while (email_sync_next_literal(response, length, &offset, &literal, &literal_len)) {
        if (headers == NULL &&
            (email_sync_find_case(literal, literal_len, "Subject:") != NULL ||
             email_sync_find_case(literal, literal_len, "From:") != NULL)) {
            headers = literal;
            headers_len = literal_len;
        } else if (body == NULL) {
            body = literal;
            body_len = literal_len;
        }
    }
    if (headers == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    char from[SOLAR_OS_EMAIL_FROM_MAX];
    char subject[SOLAR_OS_EMAIL_SUBJECT_MAX];
    char date[SOLAR_OS_EMAIL_DATE_MAX];
    char preview[SOLAR_OS_EMAIL_PREVIEW_MAX];
    email_sync_header_value(headers, headers_len, "From", from, sizeof(from));
    email_sync_header_value(headers, headers_len, "Subject", subject, sizeof(subject));
    email_sync_header_value(headers, headers_len, "Date", date, sizeof(date));
    bool truncated = body_len >= 1024U;
    email_sync_clean_preview(body != NULL ? body : "",
                             body != NULL ? body_len : 0,
                             preview,
                             sizeof(preview),
                             &truncated);
    if (subject[0] == '\0') {
        strlcpy(subject, "(no subject)", sizeof(subject));
    }
    err = solar_os_email_store(uid, from, subject, date, preview, truncated, NULL);
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

static esp_err_t email_sync_run(char *error_text, size_t error_text_len)
{
    solar_os_email_config_t config;
    esp_err_t err = solar_os_email_get_config(&config);
    if (err != ESP_OK) {
        strlcpy(error_text, "email account is not configured", error_text_len);
        return err;
    }
    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    if (!wifi.has_ip) {
        strlcpy(error_text, "Wi-Fi is not connected", error_text_len);
        return ESP_ERR_INVALID_STATE;
    }
    email_endpoint_t endpoint;
    err = email_sync_parse_url(config.url, &endpoint);
    if (err != ESP_OK) {
        strlcpy(error_text, "invalid IMAPS URL", error_text_len);
        return err;
    }

    esp_tls_t *tls = esp_tls_init();
    if (tls == NULL) {
        strlcpy(error_text, "TLS initialization failed", error_text_len);
        return ESP_ERR_NO_MEM;
    }
    esp_tls_cfg_t tls_config = {
        .non_block = true,
        .timeout_ms = EMAIL_SYNC_CONNECT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    SOLAR_OS_LOGI(TAG, "connecting to %s:%u", endpoint.host, (unsigned)endpoint.port);
    const int connected = esp_tls_conn_new_sync(endpoint.host,
                                                (int)strlen(endpoint.host),
                                                endpoint.port,
                                                &tls_config,
                                                tls);
    if (connected != 1 || email_sync.stop_requested) {
        esp_tls_conn_destroy(tls);
        strlcpy(error_text, "IMAPS connection failed", error_text_len);
        return ESP_FAIL;
    }
    err = email_sync_set_nonblocking(tls);
    if (err != ESP_OK) {
        esp_tls_conn_destroy(tls);
        strlcpy(error_text, "IMAPS socket setup failed", error_text_len);
        return err;
    }

    char *response = solar_os_memory_alloc(EMAIL_SYNC_RESPONSE_MAX,
                                           SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                           "email.response");
    if (response == NULL) {
        esp_tls_conn_destroy(tls);
        return ESP_ERR_NO_MEM;
    }

    unsigned sequence = 1;
    err = email_sync_read(tls, NULL, response, EMAIL_SYNC_RESPONSE_MAX, NULL);
    char user[SOLAR_OS_EMAIL_USER_MAX * 2U];
    char password[SOLAR_OS_EMAIL_PASSWORD_MAX * 2U];
    char mailbox[SOLAR_OS_EMAIL_MAILBOX_MAX * 2U];
    email_sync_quote(config.user, user, sizeof(user));
    email_sync_quote(config.password, password, sizeof(password));
    email_sync_quote(config.mailbox, mailbox, sizeof(mailbox));

    if (err == ESP_OK) {
        char command[sizeof(user) + sizeof(password) + 32U];
        snprintf(command, sizeof(command), "LOGIN \"%s\" \"%s\"", user, password);
        err = email_sync_command(tls, sequence++, command, response, EMAIL_SYNC_RESPONSE_MAX);
        if (err != ESP_OK) {
            strlcpy(error_text, "IMAP login failed", error_text_len);
        }
    }
    if (err == ESP_OK) {
        char command[sizeof(mailbox) + 24U];
        snprintf(command, sizeof(command), "SELECT \"%s\"", mailbox);
        err = email_sync_command(tls, sequence++, command, response, EMAIL_SYNC_RESPONSE_MAX);
        if (err == ESP_OK) {
            const uint32_t validity = email_sync_parse_uid_validity(response);
            if (validity != 0 && email_sync.uid_validity != 0 &&
                validity != email_sync.uid_validity) {
                email_sync.last_uid = 0;
                (void)solar_os_email_clear();
            }
            if (validity != 0) {
                email_sync.uid_validity = validity;
            }
        } else {
            strlcpy(error_text, "IMAP mailbox selection failed", error_text_len);
        }
    }

    uint32_t uids[EMAIL_SYNC_FETCH_MAX];
    size_t uid_count = 0;
    if (err == ESP_OK && email_sync.last_uid == 0) {
        err = email_sync_command(tls,
                                 sequence++,
                                 "UID SEARCH UID *",
                                 response,
                                 EMAIL_SYNC_RESPONSE_MAX);
        uint32_t latest[1];
        const size_t latest_count = err == ESP_OK ?
            email_sync_parse_search(response, latest, 1) : 0;
        if (latest_count > 0) {
            char command[96];
            const uint32_t first = latest[0] > EMAIL_SYNC_FETCH_MAX - 1U ?
                latest[0] - (EMAIL_SYNC_FETCH_MAX - 1U) : 1U;
            snprintf(command,
                     sizeof(command),
                     "UID SEARCH UID %lu:%lu",
                     (unsigned long)first,
                     (unsigned long)latest[0]);
            err = email_sync_command(tls, sequence++, command, response, EMAIL_SYNC_RESPONSE_MAX);
            if (err == ESP_OK) {
                uid_count = email_sync_parse_search(response, uids, EMAIL_SYNC_FETCH_MAX);
            }
        }
    } else if (err == ESP_OK) {
        char command[96];
        snprintf(command,
                 sizeof(command),
                 "UID SEARCH UID %lu:*",
                 (unsigned long)(email_sync.last_uid + 1U));
        err = email_sync_command(tls, sequence++, command, response, EMAIL_SYNC_RESPONSE_MAX);
        if (err == ESP_OK) {
            uid_count = email_sync_parse_search(response, uids, EMAIL_SYNC_FETCH_MAX);
        }
    }
    if (err != ESP_OK && error_text[0] == '\0') {
        strlcpy(error_text, "IMAP search failed", error_text_len);
    }

    for (size_t i = 0; err == ESP_OK && i < uid_count && !email_sync.stop_requested; i++) {
        err = email_sync_fetch_message(tls,
                                       sequence++,
                                       uids[i],
                                       response,
                                       EMAIL_SYNC_RESPONSE_MAX);
        if (err == ESP_OK) {
            email_sync.last_uid = uids[i];
        } else {
            strlcpy(error_text, "IMAP message fetch failed", error_text_len);
        }
    }
    if (!email_sync.stop_requested) {
        (void)email_sync_command(tls, sequence++, "LOGOUT", response, EMAIL_SYNC_RESPONSE_MAX);
    }
    solar_os_memory_free(response);
    esp_tls_conn_destroy(tls);
    return email_sync.stop_requested ? ESP_ERR_INVALID_STATE : err;
}

static void email_sync_work(void *arg)
{
    (void)arg;
    char error_text[SOLAR_OS_EMAIL_ERROR_MAX] = {0};
    solar_os_email_sync_started();
    const esp_err_t err = email_sync_run(error_text, sizeof(error_text));
    solar_os_email_sync_finished(err, error_text);
    email_sync.last_error = err;
    if (err == ESP_OK) {
        email_sync.success_count++;
        SOLAR_OS_LOGI(TAG, "sync complete; last UID %lu", (unsigned long)email_sync.last_uid);
    } else if (!email_sync.stop_requested) {
        email_sync.fail_count++;
        SOLAR_OS_LOGW(TAG, "sync failed: %s", error_text[0] != '\0' ? error_text : esp_err_to_name(err));
    }
    email_sync.sync_in_progress = false;
    if (email_sync.once) {
        email_sync.running = false;
        email_sync.complete_requested = true;
    }
    email_sync.work = (solar_os_work_handle_t)SOLAR_OS_WORK_HANDLE_INIT;
}

static esp_err_t email_sync_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (solar_os_work_active(email_sync.work) || email_sync.sync_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t interval_sec = 0;
    bool once = false;
    if (!email_sync_parse_args(argc, argv, &interval_sec, &once)) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_email_config_t config;
    esp_err_t err = solar_os_email_get_config(&config);
    if (err != ESP_OK) {
        return err;
    }
    memset(&email_sync, 0, sizeof(email_sync));
    err = solar_os_jobs_get_generation(solar_os_email_sync_job.name,
                                       &email_sync.generation);
    if (err != ESP_OK) {
        return err;
    }
    email_sync.running = true;
    email_sync.once = once;
    email_sync.interval_ms = interval_sec * 1000U;
    (void)solar_os_jobs_note_resource(solar_os_email_sync_job.name,
                                      SOLAR_OS_JOB_RESOURCE_NET,
                                      config.url,
                                      "imaps");
    return ESP_OK;
}

static void email_sync_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    email_sync.running = false;
    email_sync.stop_requested = true;
    email_sync.once = false;
    email_sync.complete_requested = false;
    if (solar_os_work_cancel(email_sync.work) == ESP_OK) {
        email_sync.work = (solar_os_work_handle_t)SOLAR_OS_WORK_HANDLE_INIT;
        email_sync.sync_in_progress = false;
    }
}

static bool email_sync_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;
    if (event == NULL || event->type != SOLAR_OS_EVENT_TICK) {
        return false;
    }
    if (email_sync.complete_requested && !email_sync.sync_in_progress) {
        email_sync.complete_requested = false;
        (void)solar_os_jobs_mark_stopped(solar_os_email_sync_job.name,
                                         email_sync.generation,
                                         email_sync.last_error);
        return true;
    }
    if (!email_sync.running || email_sync.sync_in_progress) {
        return false;
    }
    const uint32_t now = event->data.tick_ms;
    if (email_sync.next_sync_ms != 0 && (int32_t)(now - email_sync.next_sync_ms) < 0) {
        return false;
    }
    email_sync.next_sync_ms = now + email_sync.interval_ms;
    email_sync.sync_in_progress = true;
    const esp_err_t submit_err =
        solar_os_work_submit("email-sync",
                             email_sync_work,
                             NULL,
                             tskIDLE_PRIORITY + 2,
                             &email_sync.work,
                             NULL);
    if (submit_err != ESP_OK) {
        email_sync.sync_in_progress = false;
        email_sync.last_error = submit_err;
        email_sync.fail_count++;
        solar_os_email_sync_finished(submit_err, "sync work queue full");
    }
    return true;
}

const solar_os_job_t solar_os_email_sync_job = {
    .name = "email-sync",
    .summary = "periodic IMAP email synchronization",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = email_sync_start,
    .stop = email_sync_stop,
    .event = email_sync_event,
    .tick_interval_ms = 100U,
    .tick_deadline_ms = 10U,
};
