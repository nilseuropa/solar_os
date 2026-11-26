#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "solar_os_native_wireguard_abi.h"

esp_err_t solar_os_wireguard_init(void);
esp_err_t solar_os_wireguard_import(const char *path, char *detail, size_t detail_len);
esp_err_t solar_os_wireguard_forget(void);
esp_err_t solar_os_wireguard_up(solar_os_wireguard_policy_t policy);
esp_err_t solar_os_wireguard_down(void);
esp_err_t solar_os_wireguard_prepare_sleep(void);
esp_err_t solar_os_wireguard_resume(void);
void solar_os_wireguard_get_status(solar_os_wireguard_status_t *status);
const char *solar_os_wireguard_state_name(solar_os_wireguard_state_t state);
const char *solar_os_wireguard_policy_name(solar_os_wireguard_policy_t policy);

/* Platform callbacks required by the pinned wireguard-lwIP component. */
uint32_t wireguard_sys_now(void);
void wireguard_random_bytes(void *bytes, size_t size);
void wireguard_tai64n_now(uint8_t *output);
bool wireguard_is_under_load(void);
