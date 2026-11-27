#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "solar_os_identity_types_v1.h"

#define SOLAR_OS_IDENTITY_DEFAULT_USER "user"
#define SOLAR_OS_IDENTITY_DEFAULT_HOSTNAME "sol"

esp_err_t solar_os_identity_init(void);
void solar_os_identity_get(solar_os_identity_v1_t *identity);
esp_err_t solar_os_identity_set(solar_os_identity_field_t field, const char *value);
void solar_os_identity_get_user(char *buffer, size_t len);
void solar_os_identity_get_hostname(char *buffer, size_t len);
esp_err_t solar_os_identity_set_user(const char *user);
esp_err_t solar_os_identity_set_hostname(const char *hostname);
void solar_os_identity_format(char *buffer, size_t len);
