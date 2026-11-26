#pragma once

#define SOLAR_OS_IDENTITY_USER_MAX 32
#define SOLAR_OS_IDENTITY_HOSTNAME_MAX 32

typedef enum {
    SOLAR_OS_IDENTITY_USER,
    SOLAR_OS_IDENTITY_HOSTNAME,
} solar_os_identity_field_t;

typedef struct {
    char user[SOLAR_OS_IDENTITY_USER_MAX];
    char hostname[SOLAR_OS_IDENTITY_HOSTNAME_MAX];
} solar_os_identity_v1_t;
