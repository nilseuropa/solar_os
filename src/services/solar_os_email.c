#include "solar_os_email.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "solar_os_inbox.h"

#define EMAIL_NVS_NAMESPACE "email"
#define EMAIL_NVS_URL_KEY "url"
#define EMAIL_NVS_USER_KEY "user"
#define EMAIL_NVS_PASSWORD_KEY "password"
#define EMAIL_NVS_MAILBOX_KEY "mailbox"
#define EMAIL_DEFAULT_MAILBOX "INBOX"

typedef struct {
    SemaphoreHandle_t lock;
    solar_os_email_message_t *ring;
    size_t head;
    size_t count;
    size_t unread;
    uint32_t next_id;
    bool initialized;
    bool configured;
    bool ring_in_psram;
    bool syncing;
    uint32_t sync_count;
    uint32_t received_count;
    uint32_t dropped_count;
    uint32_t last_sync_ms;
    esp_err_t last_error;
    char last_error_text[SOLAR_OS_EMAIL_ERROR_MAX];
    solar_os_email_config_t config;
} email_state_t;

static email_state_t email_state = {
    .next_id = 1,
};

static void email_lock(void)
{
    if (email_state.lock != NULL) {
        (void)xSemaphoreTake(email_state.lock, portMAX_DELAY);
    }
}

static void email_unlock(void)
{
    if (email_state.lock != NULL) {
        xSemaphoreGive(email_state.lock);
    }
}

static bool email_text_valid(const char *text, size_t max_len, bool allow_empty)
{
    if (text == NULL) {
        return allow_empty;
    }
    const size_t len = strlen(text);
    if ((!allow_empty && len == 0) || len >= max_len) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p < 0x20U || *p == 0x7fU) {
            return false;
        }
    }
    return true;
}

static bool email_url_valid(const char *url)
{
    if (!email_text_valid(url, SOLAR_OS_EMAIL_URL_MAX, false) ||
        strncmp(url, "imaps://", 8) != 0) {
        return false;
    }
    const char *host = url + 8;
    return host[0] != '\0' && host[0] != ':' && strchr(host, '/') == NULL;
}

static bool email_copy(char *out, size_t out_len, const char *text)
{
    const char *value = text != NULL ? text : "";
    const bool truncated = strlen(value) >= out_len;
    strlcpy(out, value, out_len);
    return truncated;
}

static void email_clear_locked(void)
{
    if (email_state.ring != NULL) {
        memset(email_state.ring,
               0,
               SOLAR_OS_EMAIL_CAPACITY * sizeof(email_state.ring[0]));
    }
    email_state.head = 0;
    email_state.count = 0;
    email_state.unread = 0;
}

static esp_err_t email_save_config_locked(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(EMAIL_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, EMAIL_NVS_URL_KEY, email_state.config.url);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, EMAIL_NVS_USER_KEY, email_state.config.user);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, EMAIL_NVS_PASSWORD_KEY, email_state.config.password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, EMAIL_NVS_MAILBOX_KEY, email_state.config.mailbox);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void email_load_config_locked(void)
{
    nvs_handle_t nvs;
    if (nvs_open(EMAIL_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    solar_os_email_config_t config = {0};
    size_t len = sizeof(config.url);
    esp_err_t err = nvs_get_str(nvs, EMAIL_NVS_URL_KEY, config.url, &len);
    len = sizeof(config.user);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, EMAIL_NVS_USER_KEY, config.user, &len);
    }
    len = sizeof(config.password);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, EMAIL_NVS_PASSWORD_KEY, config.password, &len);
    }
    len = sizeof(config.mailbox);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, EMAIL_NVS_MAILBOX_KEY, config.mailbox, &len);
    }
    nvs_close(nvs);

    if (err == ESP_OK && email_url_valid(config.url) &&
        email_text_valid(config.user, sizeof(config.user), false) &&
        email_text_valid(config.password, sizeof(config.password), false) &&
        email_text_valid(config.mailbox, sizeof(config.mailbox), false)) {
        email_state.config = config;
        email_state.configured = true;
    }
}

esp_err_t solar_os_email_init(void)
{
    if (email_state.initialized) {
        return ESP_OK;
    }
    email_state.lock = xSemaphoreCreateMutex();
    if (email_state.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    email_state.ring = heap_caps_calloc(SOLAR_OS_EMAIL_CAPACITY,
                                        sizeof(email_state.ring[0]),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    email_state.ring_in_psram = email_state.ring != NULL;
    if (email_state.ring == NULL) {
        email_state.ring = heap_caps_calloc(SOLAR_OS_EMAIL_CAPACITY,
                                            sizeof(email_state.ring[0]),
                                            MALLOC_CAP_8BIT);
    }
    if (email_state.ring == NULL) {
        vSemaphoreDelete(email_state.lock);
        email_state.lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    email_state.initialized = true;
    email_lock();
    email_load_config_locked();
    email_unlock();
    return ESP_OK;
}

esp_err_t solar_os_email_configure(const char *url,
                                   const char *user,
                                   const char *password,
                                   const char *mailbox)
{
    esp_err_t err = solar_os_email_init();
    if (err != ESP_OK) {
        return err;
    }
    const char *selected_mailbox = mailbox != NULL && mailbox[0] != '\0' ?
        mailbox : EMAIL_DEFAULT_MAILBOX;
    if (!email_url_valid(url) ||
        !email_text_valid(user, SOLAR_OS_EMAIL_USER_MAX, false) ||
        !email_text_valid(password, SOLAR_OS_EMAIL_PASSWORD_MAX, false) ||
        !email_text_valid(selected_mailbox, SOLAR_OS_EMAIL_MAILBOX_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }

    email_lock();
    const bool changed = strcmp(email_state.config.url, url) != 0 ||
        strcmp(email_state.config.user, user) != 0 ||
        strcmp(email_state.config.mailbox, selected_mailbox) != 0;
    strlcpy(email_state.config.url, url, sizeof(email_state.config.url));
    strlcpy(email_state.config.user, user, sizeof(email_state.config.user));
    strlcpy(email_state.config.password, password, sizeof(email_state.config.password));
    strlcpy(email_state.config.mailbox, selected_mailbox, sizeof(email_state.config.mailbox));
    email_state.configured = true;
    err = email_save_config_locked();
    if (err == ESP_OK && changed) {
        email_clear_locked();
    }
    email_unlock();
    return err;
}

esp_err_t solar_os_email_forget(void)
{
    esp_err_t err = solar_os_email_init();
    if (err != ESP_OK) {
        return err;
    }
    nvs_handle_t nvs;
    err = nvs_open(EMAIL_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_all(nvs);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err == ESP_OK) {
        email_lock();
        memset(&email_state.config, 0, sizeof(email_state.config));
        email_state.configured = false;
        email_clear_locked();
        email_unlock();
    }
    return err;
}

esp_err_t solar_os_email_get_config(solar_os_email_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_email_init();
    if (err != ESP_OK) {
        return err;
    }
    email_lock();
    if (!email_state.configured) {
        email_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    *config = email_state.config;
    email_unlock();
    return ESP_OK;
}

esp_err_t solar_os_email_get_status(solar_os_email_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_email_init();
    if (err != ESP_OK) {
        return err;
    }
    email_lock();
    *status = (solar_os_email_status_t){
        .initialized = email_state.initialized,
        .configured = email_state.configured,
        .syncing = email_state.syncing,
        .ring_in_psram = email_state.ring_in_psram,
        .count = email_state.count,
        .unread = email_state.unread,
        .capacity = SOLAR_OS_EMAIL_CAPACITY,
        .sync_count = email_state.sync_count,
        .received_count = email_state.received_count,
        .dropped_count = email_state.dropped_count,
        .last_sync_ms = email_state.last_sync_ms,
        .last_error = email_state.last_error,
    };
    strlcpy(status->last_error_text, email_state.last_error_text, sizeof(status->last_error_text));
    strlcpy(status->url, email_state.config.url, sizeof(status->url));
    strlcpy(status->user, email_state.config.user, sizeof(status->user));
    strlcpy(status->mailbox, email_state.config.mailbox, sizeof(status->mailbox));
    email_unlock();
    return ESP_OK;
}

static solar_os_email_message_t *email_find_locked(uint32_t id)
{
    const size_t oldest =
        (email_state.head + SOLAR_OS_EMAIL_CAPACITY - email_state.count) % SOLAR_OS_EMAIL_CAPACITY;
    for (size_t i = 0; i < email_state.count; i++) {
        solar_os_email_message_t *message =
            &email_state.ring[(oldest + i) % SOLAR_OS_EMAIL_CAPACITY];
        if (message->id == id) {
            return message;
        }
    }
    return NULL;
}

static bool email_uid_exists_locked(uint32_t uid)
{
    const size_t oldest =
        (email_state.head + SOLAR_OS_EMAIL_CAPACITY - email_state.count) % SOLAR_OS_EMAIL_CAPACITY;
    for (size_t i = 0; i < email_state.count; i++) {
        if (email_state.ring[(oldest + i) % SOLAR_OS_EMAIL_CAPACITY].uid == uid) {
            return true;
        }
    }
    return false;
}

esp_err_t solar_os_email_store(uint32_t uid,
                               const char *from,
                               const char *subject,
                               const char *date,
                               const char *preview,
                               bool truncated,
                               uint32_t *id)
{
    if (id != NULL) {
        *id = 0;
    }
    if (uid == 0 || ((subject == NULL || subject[0] == '\0') &&
                     (preview == NULL || preview[0] == '\0'))) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_email_init();
    if (err != ESP_OK) {
        return err;
    }

    char mailbox[SOLAR_OS_EMAIL_MAILBOX_MAX];
    email_lock();
    if (email_uid_exists_locked(uid)) {
        email_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_email_message_t *message = &email_state.ring[email_state.head];
    if (email_state.count == SOLAR_OS_EMAIL_CAPACITY) {
        if (message->unread && email_state.unread > 0) {
            email_state.unread--;
        }
        email_state.dropped_count++;
    }
    memset(message, 0, sizeof(*message));
    message->id = email_state.next_id++;
    if (email_state.next_id == 0) {
        email_state.next_id = 1;
    }
    message->uid = uid;
    message->received_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    message->unread = true;
    message->truncated = truncated;
    message->truncated |= email_copy(message->from, sizeof(message->from), from);
    message->truncated |= email_copy(message->subject, sizeof(message->subject), subject);
    message->truncated |= email_copy(message->date, sizeof(message->date), date);
    message->truncated |= email_copy(message->preview, sizeof(message->preview), preview);
    strlcpy(mailbox, email_state.config.mailbox, sizeof(mailbox));
    email_state.head = (email_state.head + 1U) % SOLAR_OS_EMAIL_CAPACITY;
    if (email_state.count < SOLAR_OS_EMAIL_CAPACITY) {
        email_state.count++;
    }
    email_state.unread++;
    email_state.received_count++;
    const uint32_t local_id = message->id;
    email_unlock();

    const solar_os_inbox_publish_t notification = {
        .source = "email",
        .topic = mailbox,
        .sender = from,
        .title = subject != NULL && subject[0] != '\0' ? subject : "Email",
        .body = preview,
        .priority = SOLAR_OS_INBOX_PRIORITY_NORMAL,
    };
    uint32_t inbox_id = 0;
    if (solar_os_inbox_publish(&notification, &inbox_id) == ESP_OK) {
        email_lock();
        solar_os_email_message_t *stored = email_find_locked(local_id);
        if (stored != NULL) {
            stored->inbox_id = inbox_id;
        }
        email_unlock();
    }
    if (id != NULL) {
        *id = local_id;
    }
    return ESP_OK;
}

size_t solar_os_email_snapshot(solar_os_email_message_t *messages,
                               size_t max_messages,
                               bool unread_only,
                               size_t *total_messages)
{
    if (total_messages != NULL) {
        *total_messages = 0;
    }
    if (solar_os_email_init() != ESP_OK) {
        return 0;
    }
    email_lock();
    size_t copied = 0;
    size_t matched = 0;
    for (size_t i = 0; i < email_state.count; i++) {
        const size_t index =
            (email_state.head + SOLAR_OS_EMAIL_CAPACITY - 1U - i) % SOLAR_OS_EMAIL_CAPACITY;
        const solar_os_email_message_t *message = &email_state.ring[index];
        if (unread_only && !message->unread) {
            continue;
        }
        matched++;
        if (messages != NULL && copied < max_messages) {
            messages[copied++] = *message;
        }
    }
    if (total_messages != NULL) {
        *total_messages = matched;
    }
    email_unlock();
    return copied;
}

esp_err_t solar_os_email_get(uint32_t id, solar_os_email_message_t *message, bool mark_read)
{
    if (id == 0 || message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_email_init();
    if (err != ESP_OK) {
        return err;
    }
    uint32_t inbox_id = 0;
    email_lock();
    solar_os_email_message_t *stored = email_find_locked(id);
    if (stored == NULL) {
        email_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    *message = *stored;
    if (mark_read && stored->unread) {
        stored->unread = false;
        message->unread = false;
        if (email_state.unread > 0) {
            email_state.unread--;
        }
        inbox_id = stored->inbox_id;
    }
    email_unlock();
    if (inbox_id != 0) {
        (void)solar_os_inbox_mark_read(inbox_id, true);
    }
    return ESP_OK;
}

esp_err_t solar_os_email_mark_read(uint32_t id, bool read)
{
    esp_err_t err = solar_os_email_init();
    if (err != ESP_OK) {
        return err;
    }
    uint32_t inbox_id = 0;
    email_lock();
    solar_os_email_message_t *message = email_find_locked(id);
    if (message == NULL) {
        email_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    const bool unread = !read;
    if (message->unread != unread) {
        message->unread = unread;
        if (unread) {
            email_state.unread++;
        } else if (email_state.unread > 0) {
            email_state.unread--;
        }
    }
    inbox_id = message->inbox_id;
    email_unlock();
    if (inbox_id != 0) {
        (void)solar_os_inbox_mark_read(inbox_id, read);
    }
    return ESP_OK;
}

esp_err_t solar_os_email_clear(void)
{
    esp_err_t err = solar_os_email_init();
    if (err != ESP_OK) {
        return err;
    }
    email_lock();
    email_clear_locked();
    email_unlock();
    return ESP_OK;
}

void solar_os_email_sync_started(void)
{
    if (solar_os_email_init() != ESP_OK) {
        return;
    }
    email_lock();
    email_state.syncing = true;
    email_state.last_error = ESP_OK;
    email_state.last_error_text[0] = '\0';
    email_unlock();
}

void solar_os_email_sync_finished(esp_err_t error, const char *error_text)
{
    if (solar_os_email_init() != ESP_OK) {
        return;
    }
    email_lock();
    email_state.syncing = false;
    email_state.sync_count++;
    email_state.last_sync_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    email_state.last_error = error;
    strlcpy(email_state.last_error_text,
            error == ESP_OK ? "" : (error_text != NULL ? error_text : esp_err_to_name(error)),
            sizeof(email_state.last_error_text));
    email_unlock();
}
