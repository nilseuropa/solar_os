#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_native_email_abi.h"

esp_err_t solar_os_email_init(void);
esp_err_t solar_os_email_configure(const char *url,
                                   const char *user,
                                   const char *password,
                                   const char *mailbox);
esp_err_t solar_os_email_forget(void);
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
esp_err_t solar_os_email_sync_fetch(bool rescan, bool (*cancel)(void *), void *user);
