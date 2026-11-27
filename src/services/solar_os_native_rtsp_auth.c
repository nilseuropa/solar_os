#include "solar_os_native_rtsp_auth.h"
#include "solar_os_config.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
#include "solar_os_native_support.h"
#include "solar_os_rtsp_auth.h"

static int parse_challenge(const char *authenticate, solar_os_rtsp_auth_challenge_t *challenge)
{
    if (!challenge) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const esp_err_t error = solar_os_rtsp_auth_challenge(authenticate, challenge);
    if (error != ESP_OK) *challenge = (solar_os_rtsp_auth_challenge_t){0};
    return solar_os_native_result(error);
}

static int authorization(const solar_os_rtsp_auth_challenge_t *challenge,
    const char *method, const char *uri, const char *user, const char *password,
    char *header, size_t capacity)
{
    if (header && capacity) header[0] = 0;
    return solar_os_native_result(solar_os_rtsp_authorization(challenge, method, uri, user, password, header, capacity));
}

static int set_account(const char *host, uint16_t port, const char *user, const char *password)
{ return solar_os_native_result(solar_os_rtsp_auth_set(host, port, user, password)); }

static int get_account(const char *host, uint16_t port, char *user, size_t user_capacity,
    char *password, size_t password_capacity)
{
    return solar_os_native_result(solar_os_rtsp_auth_lookup(host, port, user, user_capacity,
        password, password_capacity));
}

static int remove_account(const char *host, uint16_t port)
{
    if (!host || !*host || !port) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(solar_os_rtsp_auth_clear(host, port));
}

static int clear_accounts(void)
{ return solar_os_native_result(solar_os_rtsp_auth_clear(NULL, 0)); }

static int list_accounts(solar_os_rtsp_auth_account_t *records, size_t capacity, size_t *count)
{ return solar_os_native_result(solar_os_rtsp_auth_list(records, capacity, count)); }

static const solar_os_native_rtsp_auth_api_v1_t rtsp_auth_api = {
    .abi_version = SOLAR_OS_NATIVE_RTSP_AUTH_ABI, .struct_size = sizeof(rtsp_auth_api),
    .parse_challenge = parse_challenge, .authorization = authorization, .set_account = set_account,
    .get_account = get_account, .remove_account = remove_account, .clear_accounts = clear_accounts,
    .list_accounts = list_accounts,
};
#endif

const void *solar_os_native_rtsp_auth_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_rtsp_auth_dispatch.inc"
    (void)name; (void)abi; (void)minimum;
    return NULL;
}
