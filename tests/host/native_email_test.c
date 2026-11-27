#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"
#include "solar_os_email.h"
#include "solar_os_inbox.h"
#include "solar_os_memory.h"
#include "solar_os_native_client.h"
#include "solar_os_native_inbox.h"
#include "solar_os_tls.h"
#include "solar_os_wifi.h"

#if SOLAR_OS_PACKAGE_SERVICE_MAIL
static const solar_os_native_email_api_v1_t *email;
static bool fail_allocation = true, fail_save;
static unsigned allocations, notifications, read_changes;
static bool inbox_read;
static bool wifi_connected;
static unsigned tls_connections;
static unsigned uid_validity = 42;
static const char *expected_search;

struct solar_os_tls_connection {
    char response[768];
    size_t offset;
};

void solar_os_wifi_get_status(solar_os_wifi_status_t *status)
{
    *status = (solar_os_wifi_status_t){.has_ip = wifi_connected};
}

esp_err_t solar_os_tls_connect(const char *host, uint16_t port, uint32_t timeout_ms,
    solar_os_tls_cancel_fn cancel, void *user, solar_os_tls_connection_t **connection)
{
    assert(!strcmp(host, "example.test") && port == 993 && timeout_ms == 15000);
    assert(!cancel(user) && !tls_connections);
    *connection = calloc(1, sizeof(**connection));
    assert(*connection);
    ++tls_connections;
    strcpy((*connection)->response, "* OK ready\r\n");
    return ESP_OK;
}

esp_err_t solar_os_tls_write(solar_os_tls_connection_t *connection, const void *buffer,
    size_t size, uint32_t timeout_ms, solar_os_tls_cancel_fn cancel, void *user, size_t *transferred)
{
    assert(connection && timeout_ms == 15000);
    if (cancel(user)) return ESP_ERR_INVALID_STATE;
    const char *command = buffer;
    char payload[512] = {0};
    if (!strncmp(command + 5, "LOGIN ", 6)) {
        assert(!strcmp(command + 5, "LOGIN \"user\" \"example-password\"\r\n"));
    } else if (!strncmp(command + 5, "SELECT ", 7)) {
        snprintf(payload, sizeof(payload), "* OK [UIDVALIDITY %u]\r\n", uid_validity);
    } else if (!strcmp(command + 5, "UID SEARCH UID *\r\n")) {
        assert(!strcmp(expected_search, "initial"));
        strcpy(payload, "* SEARCH 3\r\n");
    } else if (!strcmp(command + 5, "UID SEARCH UID 1:3\r\n")) {
        assert(!strcmp(expected_search, "initial"));
        strcpy(payload, "* SEARCH 3\r\n");
    } else if (!strcmp(command + 5, "UID SEARCH UID 4:*\r\n")) {
        assert(!strcmp(expected_search, "incremental"));
        strcpy(payload, "* SEARCH\r\n");
    } else if (!strncmp(command + 5, "UID FETCH 3 ", 12)) {
        static const char header[] = "From: sender\r\nSubject: fetched\r\n\r\n";
        static const char body[] = "Hello <b>there</b>";
        snprintf(payload, sizeof(payload), "* 1 FETCH ({%zu}\r\n%s {%zu}\r\n%s)\r\n",
            sizeof(header) - 1, header, sizeof(body) - 1, body);
    } else {
        assert(!strcmp(command + 5, "LOGOUT\r\n"));
    }
    snprintf(connection->response, sizeof(connection->response), "%s%.4s OK done\r\n", payload, command);
    connection->offset = 0;
    *transferred = size;
    return ESP_OK;
}

esp_err_t solar_os_tls_read(solar_os_tls_connection_t *connection, void *buffer,
    size_t size, uint32_t timeout_ms, solar_os_tls_cancel_fn cancel, void *user, size_t *transferred)
{
    assert(connection && timeout_ms == 15000);
    if (cancel(user)) return ESP_ERR_INVALID_STATE;
    const size_t available = strlen(connection->response + connection->offset);
    const size_t chunk = size < 3 ? size : 3;
    *transferred = available < chunk ? available : chunk;
    memcpy(buffer, connection->response + connection->offset, *transferred);
    connection->offset += *transferred;
    return ESP_OK;
}

void solar_os_tls_close(solar_os_tls_connection_t *connection)
{
    assert(tls_connections == 1 && connection);
    --tls_connections;
    free(connection);
}

void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{
    assert(size == 8192 && cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && !strcmp(tag, "email.response"));
    return malloc(size);
}

void solar_os_memory_free(void *pointer) { free(pointer); }

static bool cancel_fetch(void *user)
{
    unsigned *checks = user;
    solar_os_email_status_t status;
    assert(!email->status(&status) && status.syncing);
    assert(email->fetch(false, NULL, NULL) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(email->forget() == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(email->configure("imaps://replacement.test", "other", "other-pass", NULL) ==
        SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    return ++*checks == 3;
}

void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && !strcmp(tag, "email.ring"));
    ++allocations;
    if (fail_allocation) return NULL;
    return calloc(count, size);
}

bool solar_os_memory_is_external(const void *pointer) { return pointer != NULL; }
int64_t esp_timer_get_time(void) { return 42000; }
const char *esp_err_to_name(esp_err_t error)
{
    assert(error == ESP_ERR_TIMEOUT);
    return "ESP_ERR_TIMEOUT";
}

size_t strlcpy(char *out, const char *in, size_t size)
{
    const size_t length = strlen(in);
    if (size) {
        const size_t copied = length < size - 1 ? length : size - 1;
        memcpy(out, in, copied);
        out[copied] = 0;
    }
    return length;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, "email"));
    if (mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *length)
{
    (void)handle;
    (void)key;
    (void)value;
    (void)length;
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    assert(handle == 1 && key && value && value[0]);
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle) { assert(handle == 1); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle) { assert(handle == 1); return fail_save ? ESP_FAIL : ESP_OK; }
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }

esp_err_t solar_os_inbox_publish(const solar_os_inbox_publish_t *notification, uint32_t *id)
{
    assert(!strcmp(notification->source, "email") && notification->dedupe_key);
    *id = ++notifications;
    return ESP_OK;
}

esp_err_t solar_os_inbox_mark_read(uint32_t id, bool read)
{
    assert(id && id <= notifications);
    ++read_changes;
    inbox_read = read;
    return ESP_OK;
}

static void *check_status(void *arg)
{
    assert(arg == NULL);
    solar_os_email_status_t status;
    assert(!email->status(&status) && status.initialized && status.ring_in_psram);
    return NULL;
}
#endif

int main(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_MAIL
    email = solar_os_native_email_get(solar_os_native_inbox_get_service, sizeof(*email));
    assert(email && !solar_os_native_inbox_get_service("email", 2, sizeof(*email)));
    solar_os_email_status_t status;
    assert(email->status(&status) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
    fail_allocation = false;
    pthread_t readers[4];
    for (size_t i = 0; i < 4; ++i) assert(!pthread_create(&readers[i], NULL, check_status, NULL));
    for (size_t i = 0; i < 4; ++i) assert(!pthread_join(readers[i], NULL));
    assert(allocations == 2);

    assert(!email->configure("imaps://example.test", "user", "example-password", NULL));
    assert(!email->status(&status) && status.configured && !strcmp(status.mailbox, "INBOX"));
    assert(email->fetch(false, NULL, NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!email->status(&status) && status.last_error == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !status.syncing);
    uint32_t first, second;
    assert(!email->store(1, "from", "first", "date", "preview", false, &first) && first);
    assert(!email->store(2, NULL, "second", NULL, NULL, true, &second) && second != first);
    assert(email->store(1, "from", "first", "date", "preview", false, NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(notifications == 2);
    size_t total;
    solar_os_email_message_t message;
    assert(email->snapshot(&message, 1, true, &total) == 1 && total == 2 && message.id == second && message.truncated);
    assert(!email->get(first, &message, true) && !message.unread && inbox_read && read_changes == 1);
    assert(!email->mark_read(first, false) && !inbox_read && read_changes == 2);

    fail_save = true;
    assert(email->configure("imaps://replacement.test", "changed", "changed", "Archive") == SOLAR_OS_NATIVE_ERROR_FAILED);
    assert(!email->status(&status) && !strcmp(status.url, "imaps://example.test") && status.count == 2);
    assert(email->forget() == SOLAR_OS_NATIVE_ERROR_FAILED);
    assert(!email->status(&status) && status.configured);
    fail_save = false;
    assert(!email->clear());
    assert(email->get(first, &message, false) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    wifi_connected = true;
    unsigned checks = 0;
    assert(email->fetch(false, cancel_fetch, &checks) == SOLAR_OS_NATIVE_ERROR_CANCELLED);
    assert(!email->status(&status) && status.last_error == SOLAR_OS_NATIVE_ERROR_CANCELLED &&
        !status.syncing && checks == 3 && !tls_connections);
    expected_search = "initial";
    assert(!email->fetch(false, NULL, NULL));
    assert(email->snapshot(&message, 1, false, &total) == 1 && total == 1);
    assert(message.uid == 3 && !strcmp(message.subject, "fetched") && !strcmp(message.preview, "Hello there"));
    expected_search = "incremental";
    assert(!email->fetch(false, NULL, NULL) && !tls_connections && notifications == 3);
    uid_validity++;
    expected_search = "initial";
    assert(!email->fetch(true, NULL, NULL) && !tls_connections && notifications == 4);
    assert(!email->forget() && !email->status(&status) && !status.configured);
#else
    assert(!solar_os_native_email_get(solar_os_native_inbox_get_service, sizeof(solar_os_native_email_api_v1_t)));
#endif
    return 0;
}
