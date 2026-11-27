#include "solar_os_email_sync_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_timer.h"
#include "solar_os_memory.h"
#include "solar_os_tls.h"
#include "solar_os_wifi.h"

#define EMAIL_SYNC_DEFAULT_PORT 993U
#define EMAIL_SYNC_CONNECT_TIMEOUT_MS 15000U
#define EMAIL_SYNC_IO_TIMEOUT_MS 15000U
#define EMAIL_SYNC_RESPONSE_MAX 8192U
#define EMAIL_SYNC_FETCH_MAX 8U
#define EMAIL_SYNC_HOST_MAX 128U

typedef struct {
    char host[EMAIL_SYNC_HOST_MAX];
    uint16_t port;
} email_endpoint_t;

typedef struct {
    solar_os_tls_connection_t *connection;
    bool (*cancel)(void *);
    void *user;
    bool cancelled;
} email_sync_connection_t;

static struct {
    uint32_t revision;
    uint32_t last_uid;
    uint32_t uid_validity;
} email_cursor;

static bool email_sync_cancelled(void *user)
{
    email_sync_connection_t *sync = user;
    if (!sync->cancelled && sync->cancel) {
        sync->cancelled = sync->cancel(sync->user);
    }
    return sync->cancelled;
}

static uint32_t email_sync_time_remaining(int64_t deadline)
{
    const int64_t remaining = deadline - esp_timer_get_time();
    return remaining > 0 ? (uint32_t)((remaining + 999) / 1000) : 0;
}

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

static esp_err_t email_sync_write_all(email_sync_connection_t *sync, const char *text)
{
    size_t offset = 0;
    const size_t length = strlen(text);
    const int64_t deadline = esp_timer_get_time() + EMAIL_SYNC_IO_TIMEOUT_MS * 1000LL;
    while (offset < length) {
        const uint32_t remaining = email_sync_time_remaining(deadline);
        if (!remaining) return ESP_ERR_TIMEOUT;
        size_t written;
        const esp_err_t error = solar_os_tls_write(sync->connection, text + offset, length - offset,
            remaining, email_sync_cancelled, sync, &written);
        if (error != ESP_OK) return error;
        offset += written;
    }
    return ESP_OK;
}

static bool email_sync_tag_complete(const char *response, const char *tag, bool *ok)
{
    if (response == NULL || tag == NULL) {
        return false;
    }
    const size_t tag_len = strlen(tag);
    const char *line = response;
    for (const char *end; (end = strchr(line, '\n')) != NULL; line = end + 1) {
        if ((size_t)(end - line) > tag_len &&
            strncmp(line, tag, tag_len) == 0 && line[tag_len] == ' ') {
            const char *status = line + tag_len + 1U;
            *ok = strncasecmp(status, "OK", 2) == 0 &&
                (status[2] == ' ' || status[2] == '\r' || status[2] == '\n');
            return true;
        }
    }
    return false;
}

static esp_err_t email_sync_read(email_sync_connection_t *sync, const char *tag,
    char *response, size_t response_len)
{
    size_t used = 0;
    const int64_t deadline = esp_timer_get_time() + EMAIL_SYNC_IO_TIMEOUT_MS * 1000LL;
    response[0] = '\0';
    for (;;) {
        const uint32_t remaining = email_sync_time_remaining(deadline);
        if (!remaining) return ESP_ERR_TIMEOUT;
        if (used + 1U >= response_len) return ESP_ERR_INVALID_SIZE;
        size_t read;
        const esp_err_t error = solar_os_tls_read(sync->connection, response + used,
            response_len - used - 1U, remaining, email_sync_cancelled, sync, &read);
        if (error != ESP_OK) return error;
        if (!read) return ESP_ERR_INVALID_STATE;
        used += read;
        response[used] = '\0';
        if (!tag && strchr(response, '\n')) {
            return strncmp(response, "* OK", 4) == 0 ? ESP_OK : ESP_FAIL;
        }
        bool ok;
        if (tag && email_sync_tag_complete(response, tag, &ok)) return ok ? ESP_OK : ESP_FAIL;
    }
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

static esp_err_t email_sync_command(email_sync_connection_t *sync,
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
        explicit_bzero(line, sizeof(line));
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_err_t error = email_sync_write_all(sync, line);
    explicit_bzero(line, sizeof(line));
    return error == ESP_OK ? email_sync_read(sync, tag, response, response_len) : error;
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

static esp_err_t email_sync_fetch_message(email_sync_connection_t *sync,
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
    esp_err_t err = email_sync_command(sync, sequence, command, response, response_len);
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
    if (email_sync_cancelled(sync)) return ESP_ERR_INVALID_STATE;
    err = solar_os_email_store(uid, from, subject, date, preview, truncated, NULL);
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

static esp_err_t email_sync_run(email_sync_connection_t *sync, const solar_os_email_account_t *account,
    char *error_text, size_t error_text_len)
{
    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    if (!wifi.has_ip) {
        strlcpy(error_text, "Wi-Fi is not connected", error_text_len);
        return ESP_ERR_INVALID_STATE;
    }
    email_endpoint_t endpoint;
    esp_err_t err = email_sync_parse_url(account->url, &endpoint);
    if (err != ESP_OK) {
        strlcpy(error_text, "invalid IMAPS URL", error_text_len);
        return err;
    }

    err = solar_os_tls_connect(endpoint.host, endpoint.port, EMAIL_SYNC_CONNECT_TIMEOUT_MS,
        email_sync_cancelled, sync, &sync->connection);
    if (err != ESP_OK) {
        strlcpy(error_text, "IMAPS connection failed", error_text_len);
        return err;
    }

    char *response = solar_os_memory_alloc(EMAIL_SYNC_RESPONSE_MAX,
                                           SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                           "email.response");
    if (response == NULL) {
        solar_os_tls_close(sync->connection);
        return ESP_ERR_NO_MEM;
    }

    unsigned sequence = 1;
    err = email_sync_read(sync, NULL, response, EMAIL_SYNC_RESPONSE_MAX);
    if (err == ESP_OK) {
        char user[SOLAR_OS_EMAIL_USER_MAX * 2U];
        char password[SOLAR_OS_EMAIL_PASSWORD_MAX * 2U];
        email_sync_quote(account->user, user, sizeof(user));
        email_sync_quote(account->password, password, sizeof(password));
        char command[sizeof(user) + sizeof(password) + 32U];
        snprintf(command, sizeof(command), "LOGIN \"%s\" \"%s\"", user, password);
        err = email_sync_command(sync, sequence++, command, response, EMAIL_SYNC_RESPONSE_MAX);
        explicit_bzero(command, sizeof(command));
        explicit_bzero(password, sizeof(password));
        if (err != ESP_OK) {
            strlcpy(error_text, "IMAP login failed", error_text_len);
        }
    }
    if (err == ESP_OK) {
        char mailbox[SOLAR_OS_EMAIL_MAILBOX_MAX * 2U];
        email_sync_quote(account->mailbox, mailbox, sizeof(mailbox));
        char command[sizeof(mailbox) + 24U];
        snprintf(command, sizeof(command), "SELECT \"%s\"", mailbox);
        err = email_sync_command(sync, sequence++, command, response, EMAIL_SYNC_RESPONSE_MAX);
        if (err == ESP_OK) {
            const uint32_t validity = email_sync_parse_uid_validity(response);
            if (validity != 0 && email_cursor.uid_validity != 0 &&
                validity != email_cursor.uid_validity) {
                email_cursor.last_uid = 0;
                (void)solar_os_email_clear();
            }
            if (validity != 0) {
                email_cursor.uid_validity = validity;
            }
        } else {
            strlcpy(error_text, "IMAP mailbox selection failed", error_text_len);
        }
    }

    uint32_t uids[EMAIL_SYNC_FETCH_MAX];
    size_t uid_count = 0;
    if (err == ESP_OK && email_cursor.last_uid == 0) {
        err = email_sync_command(sync,
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
            err = email_sync_command(sync, sequence++, command, response, EMAIL_SYNC_RESPONSE_MAX);
            if (err == ESP_OK) {
                uid_count = email_sync_parse_search(response, uids, EMAIL_SYNC_FETCH_MAX);
            }
        }
    } else if (err == ESP_OK) {
        char command[96];
        snprintf(command,
                 sizeof(command),
                 "UID SEARCH UID %lu:*",
                 (unsigned long)(email_cursor.last_uid + 1U));
        err = email_sync_command(sync, sequence++, command, response, EMAIL_SYNC_RESPONSE_MAX);
        if (err == ESP_OK) {
            uid_count = email_sync_parse_search(response, uids, EMAIL_SYNC_FETCH_MAX);
        }
    }
    if (err != ESP_OK && error_text[0] == '\0') {
        strlcpy(error_text, "IMAP search failed", error_text_len);
    }

    for (size_t i = 0; err == ESP_OK && i < uid_count && !email_sync_cancelled(sync); i++) {
        err = email_sync_fetch_message(sync,
                                       sequence++,
                                       uids[i],
                                       response,
                                       EMAIL_SYNC_RESPONSE_MAX);
        if (err == ESP_OK) {
            email_cursor.last_uid = uids[i];
        } else {
            strlcpy(error_text, "IMAP message fetch failed", error_text_len);
        }
    }
    if (!email_sync_cancelled(sync)) {
        (void)email_sync_command(sync, sequence++, "LOGOUT", response, EMAIL_SYNC_RESPONSE_MAX);
    }
    explicit_bzero(response, EMAIL_SYNC_RESPONSE_MAX);
    solar_os_memory_free(response);
    solar_os_tls_close(sync->connection);
    return err;
}

esp_err_t solar_os_email_sync_fetch(bool rescan, bool (*cancel)(void *), void *user)
{
    solar_os_email_account_t account;
    uint32_t revision;
    esp_err_t error = solar_os_email_sync_begin(&account, &revision);
    if (error != ESP_OK) return error;
    if (email_cursor.revision != revision) {
        email_cursor.revision = revision;
        email_cursor.last_uid = 0;
        email_cursor.uid_validity = 0;
    } else if (rescan) {
        email_cursor.last_uid = 0;
    }
    email_sync_connection_t sync = {.cancel = cancel, .user = user};
    char error_text[SOLAR_OS_EMAIL_ERROR_MAX] = {0};
    if (!email_sync_cancelled(&sync)) {
        error = email_sync_run(&sync, &account, error_text, sizeof(error_text));
    }
    explicit_bzero(&account, sizeof(account));
    if (sync.cancelled) {
        error = ESP_ERR_NOT_ALLOWED;
        strlcpy(error_text, "email sync cancelled", sizeof(error_text));
    }
    solar_os_email_sync_finished(error, error_text);
    return error;
}
