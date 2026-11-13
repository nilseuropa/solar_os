#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_SSH_KEY_DEFAULT_BITS 3072U
#define SOLAR_OS_SSH_KEY_MIN_BITS 2048U
#define SOLAR_OS_SSH_KEY_MAX_BITS 4096U
#define SOLAR_OS_SSH_PUBLIC_KEY_MAX 1280U

typedef struct {
    bool private_key_exists;
    bool public_key_exists;
    uint32_t private_key_size;
    uint32_t public_key_size;
    char private_key_path[160];
    char public_key_path[160];
} solar_os_ssh_key_status_t;

esp_err_t solar_os_ssh_keys_default_paths(char *private_key_path,
                                          size_t private_key_path_len,
                                          char *public_key_path,
                                          size_t public_key_path_len);
bool solar_os_ssh_keys_default_exists(void);
esp_err_t solar_os_ssh_keys_get_status(solar_os_ssh_key_status_t *status);
esp_err_t solar_os_ssh_keys_read_public(char *public_key,
                                        size_t public_key_len,
                                        size_t *written);
esp_err_t solar_os_ssh_keys_generate_rsa(uint32_t bits, bool overwrite);
esp_err_t solar_os_ssh_keys_remove_default(void);
