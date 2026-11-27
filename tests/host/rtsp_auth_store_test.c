#include <pthread.h>
#include <stddef.h>

#include "nvs.h"
#include "solar_os_native_rtsp_auth.h"
#include "solar_os_native_support.h"

#define main existing_auth_vectors
#include "rtsp_auth_test.c"
#undef main

/* The on-disk format dates from before the native account table. */
typedef struct {
    char host[64];
    uint16_t port;
    char user[65], password[65];
} legacy_record_t;

static legacy_record_t saved[4], staged[4];
static bool fail_get, fail_set, fail_commit;
static pthread_mutex_t nvs_lock = PTHREAD_MUTEX_INITIALIZER;

esp_err_t nvs_open(const char *namespace, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(namespace, "rtsp_auth"));
    /* Production must retain exclusive access through commit and close. */
    assert(!pthread_mutex_trylock(&nvs_lock));
    *handle = mode + 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *length)
{
    assert(handle == NVS_READONLY + 1 && !strcmp(key, "accounts") && *length == sizeof(saved));
    if (fail_get) return ESP_FAIL;
    memcpy(out, saved, sizeof(saved));
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    assert(handle == NVS_READWRITE + 1 && !strcmp(key, "accounts") && length == sizeof(staged));
    if (fail_set) return ESP_FAIL;
    memcpy(staged, value, length);
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == NVS_READWRITE + 1);
    if (fail_commit) return ESP_FAIL;
    memcpy(saved, staged, sizeof(saved));
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle);
    assert(!pthread_mutex_unlock(&nvs_lock));
}

int solar_os_native_error_result(esp_err_t error)
{
    return solar_os_native_result_value(error);
}

static const solar_os_native_rtsp_auth_api_v1_t *api;

static void read_password(const char *host, const char *expected)
{
    char user[65], secret[65];
    assert(api->get_account(host, 554, user, sizeof(user), secret, sizeof(secret)) == SOLAR_OS_NATIVE_OK);
    assert(!strcmp(user, "viewer") && !strcmp(secret, expected));
}

static void *add_camera(void *host)
{
    assert(api->set_account(host, 554, "viewer", host) == SOLAR_OS_NATIVE_OK);
    return NULL;
}

static void test_store(void)
{
    api = solar_os_native_rtsp_auth_get_service(SOLAR_OS_NATIVE_RTSP_AUTH_SERVICE,
        SOLAR_OS_NATIVE_RTSP_AUTH_ABI, sizeof(*api));
    assert(api && api->struct_size == sizeof(*api));
    saved[0] = (legacy_record_t){.host = "camera.local", .port = 554, .user = "viewer", .password = "original"};
    size_t count = 20;
    fail_get = true;
    assert(api->list_accounts(NULL, 0, &count) == SOLAR_OS_NATIVE_ERROR_FAILED && count == 0);
    fail_get = false;
    memset(saved[0].password, 'x', sizeof(saved[0].password));
    assert(api->list_accounts(NULL, 0, &count) == SOLAR_OS_NATIVE_ERROR_FAILED && count == 0);
    memcpy(saved[0].password, "original", sizeof("original"));
    assert(api->list_accounts(NULL, 0, &count) == SOLAR_OS_NATIVE_OK && count == 1);
    read_password("CAMERA.LOCAL", "original");

    fail_set = true;
    assert(api->set_account("camera.local", 554, "viewer", "replacement") == SOLAR_OS_NATIVE_ERROR_FAILED);
    read_password("camera.local", "original");
    fail_set = false;
    fail_commit = true;
    assert(api->clear_accounts() == SOLAR_OS_NATIVE_ERROR_FAILED);
    assert(api->remove_account("camera.local", 554) == SOLAR_OS_NATIVE_ERROR_FAILED);
    read_password("camera.local", "original");
    assert(!strcmp(saved[0].password, "original"));
    fail_commit = false;
    assert(api->set_account("camera.local", 554, "viewer", "replacement") == SOLAR_OS_NATIVE_OK);
    read_password("camera.local", "replacement");

    pthread_t threads[2];
    assert(!pthread_create(&threads[0], NULL, add_camera, "east.local"));
    assert(!pthread_create(&threads[1], NULL, add_camera, "west.local"));
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
    read_password("east.local", "east.local");
    read_password("west.local", "west.local");
    assert(api->list_accounts(NULL, 0, &count) == SOLAR_OS_NATIVE_OK && count == 3);
    solar_os_rtsp_auth_account_t listed[1];
    assert(api->list_accounts(listed, 1, &count) == SOLAR_OS_NATIVE_OK && count == 1);
    assert(!strcmp(listed[0].host, "camera.local") && !strcmp(listed[0].user, "viewer"));
    assert(api->remove_account("", 554) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->clear_accounts() == SOLAR_OS_NATIVE_OK);
    assert(api->list_accounts(NULL, 0, &count) == SOLAR_OS_NATIVE_OK && !count);
}

static void test_native_challenge(void)
{
    solar_os_rtsp_auth_challenge_t challenge;
    const char *text = "Basic realm=\"camera\"";
    assert(api->parse_challenge(text, &challenge) == SOLAR_OS_NATIVE_OK && !challenge.digest);
    char header[128];
    assert(api->authorization(&challenge, "DESCRIBE", "rtsp://camera.local/media", "viewer", "example",
        header, sizeof(header)) == SOLAR_OS_NATIVE_OK);
    assert(!strcmp(header, "Basic dmlld2VyOmV4YW1wbGU="));
    challenge.digest = true;
    memset(challenge.realm, 'a', sizeof(challenge.realm));
    assert(api->authorization(&challenge, "DESCRIBE", "rtsp://camera.local/media", "viewer", "example",
        header, sizeof(header)) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!header[0]);
}

int main(void)
{
    test_store();
    test_native_challenge();
    existing_auth_vectors();
    puts("rtsp_auth_store_test: OK");
    return 0;
}
