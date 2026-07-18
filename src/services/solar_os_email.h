#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_EMAIL_CAPACITY 32U
#define SOLAR_OS_EMAIL_URL_MAX 160U
#define SOLAR_OS_EMAIL_USER_MAX 96U
#define SOLAR_OS_EMAIL_PASSWORD_MAX 128U
#define SOLAR_OS_EMAIL_MAILBOX_MAX 64U
#define SOLAR_OS_EMAIL_FROM_MAX 96U
#define SOLAR_OS_EMAIL_SUBJECT_MAX 128U
#define SOLAR_OS_EMAIL_DATE_MAX 48U
#define SOLAR_OS_EMAIL_PREVIEW_MAX 768U
#define SOLAR_OS_EMAIL_ERROR_MAX 96U

typedef struct {
    char url[SOLAR_OS_EMAIL_URL_MAX];
    char user[SOLAR_OS_EMAIL_USER_MAX];
    char password[SOLAR_OS_EMAIL_PASSWORD_MAX];
    char mailbox[SOLAR_OS_EMAIL_MAILBOX_MAX];
} solar_os_email_config_t;

typedef struct {
    uint32_t id;
    uint32_t uid;
    uint32_t inbox_id;
    uint32_t received_ms;
    bool unread;
    bool truncated;
    char from[SOLAR_OS_EMAIL_FROM_MAX];
    char subject[SOLAR_OS_EMAIL_SUBJECT_MAX];
    char date[SOLAR_OS_EMAIL_DATE_MAX];
    char preview[SOLAR_OS_EMAIL_PREVIEW_MAX];
} solar_os_email_message_t;

typedef struct {
    bool initialized;
    bool configured;
    bool syncing;
    bool ring_in_psram;
    size_t count;
    size_t unread;
    size_t capacity;
    uint32_t sync_count;
    uint32_t received_count;
    uint32_t dropped_count;
    uint32_t last_sync_ms;
    esp_err_t last_error;
    char last_error_text[SOLAR_OS_EMAIL_ERROR_MAX];
    char url[SOLAR_OS_EMAIL_URL_MAX];
    char user[SOLAR_OS_EMAIL_USER_MAX];
    char mailbox[SOLAR_OS_EMAIL_MAILBOX_MAX];
} solar_os_email_status_t;

esp_err_t solar_os_email_init(void);
esp_err_t solar_os_email_configure(const char *url,
                                   const char *user,
                                   const char *password,
                                   const char *mailbox);
esp_err_t solar_os_email_forget(void);
esp_err_t solar_os_email_get_config(solar_os_email_config_t *config);
esp_err_t solar_os_email_get_status(solar_os_email_status_t *status);
esp_err_t solar_os_email_store(uint32_t uid,
                               const char *from,
                               const char *subject,
                               const char *date,
                               const char *preview,
                               bool truncated,
                               uint32_t *id);
size_t solar_os_email_snapshot(solar_os_email_message_t *messages,
                               size_t max_messages,
                               bool unread_only,
                               size_t *total_messages);
esp_err_t solar_os_email_get(uint32_t id, solar_os_email_message_t *message, bool mark_read);
esp_err_t solar_os_email_mark_read(uint32_t id, bool read);
esp_err_t solar_os_email_clear(void);
void solar_os_email_sync_started(void);
void solar_os_email_sync_finished(esp_err_t error, const char *error_text);
