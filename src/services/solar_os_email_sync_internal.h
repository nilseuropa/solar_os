#pragma once

#include "solar_os_email.h"

typedef struct {
    char url[SOLAR_OS_EMAIL_URL_MAX];
    char user[SOLAR_OS_EMAIL_USER_MAX];
    char password[SOLAR_OS_EMAIL_PASSWORD_MAX];
    char mailbox[SOLAR_OS_EMAIL_MAILBOX_MAX];
} solar_os_email_account_t;

/* Reserve the configured account for a single fetch. The paired finish
 * releases the reservation and publishes its final status. */
esp_err_t solar_os_email_sync_begin(solar_os_email_account_t *account, uint32_t *revision);
void solar_os_email_sync_finished(esp_err_t error, const char *error_text);
