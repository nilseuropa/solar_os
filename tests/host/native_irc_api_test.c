#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_irc.h"
#include "solar_os_irc_protocol.h"
#include "solar_os_memory.h"
#include "solar_os_native_irc.h"
#include "solar_os_sync.h"

typedef void *TaskHandle_t;

typedef struct {
    solar_os_messaging_provider_id_t id;
    solar_os_irc_config_t config;
    uint32_t revision;
} irc_instance_t;

static struct {
    atomic_bool initialized, running, stop_requested;
    SemaphoreHandle_t lock, edit_lock;
    TaskHandle_t worker;
} irc;
static solar_os_lifecycle_t irc_lifecycle;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t edit_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint allocations;
static bool fail_persist, fail_worker, linger;
static unsigned worker_creations, worker_notifications;
static TickType_t ticks;
static irc_instance_t instance = {
    .id = 3,
    .config = {.version = 1, .port = 6697, .host = "server.invalid", .nick = "original"},
};

#define tskIDLE_PRIORITY 0
#define tskNO_AFFINITY 0
#define SOLAR_OS_TASK_ROLE_BACKGROUND 1
#define pdPASS pdTRUE
#define pdMS_TO_TICKS(ms) (ms)
#define pdTICKS_TO_MS(ticks) (ticks)

void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t kind, const char *tag)
{
    assert(count == 1 && size == sizeof(solar_os_irc_config_t) && kind == SOLAR_OS_MEMORY_EXTERNAL_REQUIRED && tag);
    void *memory = calloc(count, size);
    if (memory) allocations++;
    return memory;
}

void solar_os_memory_free(void *memory)
{
    if (memory) {
        const unsigned char *bytes = memory;
        for (size_t i = 0; i < sizeof(solar_os_irc_config_t); i++) assert(bytes[i] == 0);
        assert(allocations);
        allocations--;
        free(memory);
    }
}

size_t strlcpy(char *out, const char *in, size_t capacity)
{
    const size_t len = strlen(in);
    if (capacity) {
        const size_t copied = len < capacity - 1U ? len : capacity - 1U;
        memcpy(out, in, copied);
        out[copied] = '\0';
    }
    return len;
}

esp_err_t solar_os_irc_init(void)
{
    if (!atomic_load(&irc.initialized)) {
        irc.lock = &lock;
        irc.edit_lock = &edit_lock;
        atomic_store(&irc.initialized, true);
    }
    return ESP_OK;
}

static irc_instance_t *irc_find(solar_os_messaging_provider_id_t id)
{
    return id == instance.id ? &instance : NULL;
}

bool solar_os_messaging_provider_is_class(solar_os_messaging_provider_id_t id, const char *name)
{
    return id == instance.id && !strcmp(name, "irc");
}

esp_err_t solar_os_messaging_profile_load(solar_os_messaging_provider_id_t id, void *out, size_t length)
{
    assert(id == instance.id && length == sizeof(instance.config));
    memcpy(out, &instance.config, length);
    return ESP_OK;
}

esp_err_t solar_os_messaging_profile_save(solar_os_messaging_provider_id_t id, const void *config, size_t length)
{
    assert(id == instance.id && config && length == sizeof(instance.config));
    return fail_persist ? ESP_FAIL : ESP_OK;
}

static esp_err_t irc_attach(solar_os_messaging_provider_id_t id, const solar_os_irc_config_t *config)
{
    assert(id == instance.id && config);
    return ESP_OK;
}

static void xTaskNotifyGive(TaskHandle_t worker)
{
    assert(worker == irc.worker);
    worker_notifications++;
}

static esp_err_t esp_netif_init(void) { return ESP_OK; }
static void irc_worker(void *arg) { (void)arg; assert(false); }

static int solar_os_task_create_pinned(void (*worker)(void *), const char *name,
    size_t stack, void *arg, unsigned priority, TaskHandle_t *out, unsigned affinity, unsigned role)
{
    assert(worker == irc_worker && !strcmp(name, "irc_sync") && stack == 8192 && !arg);
    assert(priority == tskIDLE_PRIORITY + 1 && affinity == tskNO_AFFINITY && role == SOLAR_OS_TASK_ROLE_BACKGROUND);
    worker_creations++;
    if (fail_worker) return pdFALSE;
    *out = &irc_lifecycle;
    return pdPASS;
}

static TickType_t xTaskGetTickCount(void) { return ticks; }

static void vTaskDelay(TickType_t delay)
{
    ticks += delay;
    if (!linger && atomic_load(&irc.stop_requested)) {
        xSemaphoreTake(irc.lock, portMAX_DELAY);
        irc.worker = NULL;
        atomic_store(&irc.running, false);
        xSemaphoreGive(irc.lock);
    }
}

#include "irc_core.inc"

typedef struct {
    const solar_os_native_irc_api_v1_t *api;
    solar_os_native_irc_update_v1_t update;
} update_fixture_t;

static void *update_profile(void *arg)
{
    const update_fixture_t *fixture = arg;
    assert(!fixture->api->configure(3, &fixture->update));
    return NULL;
}

int main(void)
{
    const solar_os_native_irc_api_v1_t *api = solar_os_native_irc_get_service("messages.irc", 1, sizeof(*api));
    assert(api && !api->running() && !api->stop());
    solar_os_native_irc_profile_v1_t profile;
    assert(!api->profile(3, &profile) && profile.port == 6697 && !profile.enabled);
    assert(!strcmp(profile.nick, "original") && !profile.sasl_password_set);
    assert(api->profile(5, &profile) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !profile.host[0]);

    const char *const channels[] = {"#one", "#two"};
    solar_os_native_irc_update_v1_t update = {
        .account = "account", .password = "password", .server_password = "server-secret",
        .channels = channels, .channel_count = 2, .replace_channels = true,
        .set_enabled = true, .enabled = true,
    };
    assert(!api->configure(3, &update));
    assert(!api->profile(3, &profile) && profile.enabled && profile.sasl_password_set && profile.server_password_set);
    assert(!strcmp(profile.channels[1], "#two") && !profile.channels[2][0]);

    update = (solar_os_native_irc_update_v1_t){.nick = "unsaved"};
    fail_persist = true;
    const uint32_t before = instance.revision;
    assert(api->configure(3, &update) == SOLAR_OS_NATIVE_ERROR_FAILED && instance.revision == before);
    fail_persist = false;
    assert(!api->profile(3, &profile) && !strcmp(profile.nick, "original"));
    assert(api->configure(3, &(solar_os_native_irc_update_v1_t){.port = 65536}) ==
           SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->configure(3, &(solar_os_native_irc_update_v1_t){.nick = "invalid nick"}) ==
           SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);

    const update_fixture_t changes[] = {
        {.api = api, .update = {.host = "new.invalid"}},
        {.api = api, .update = {.nick = "newnick", .password = ""}},
    };
    pthread_t writers[2];
    for (size_t i = 0; i < 2; i++)
        assert(!pthread_create(&writers[i], NULL, update_profile, (void *)&changes[i]));
    for (size_t i = 0; i < 2; i++) assert(!pthread_join(writers[i], NULL));
    assert(!api->profile(3, &profile) && !strcmp(profile.host, "new.invalid") && !strcmp(profile.nick, "newnick"));
    assert(!profile.account[0] && !profile.sasl_password_set && profile.server_password_set);
    assert(!api->configure(3, &(solar_os_native_irc_update_v1_t){.replace_channels = true}));
    assert(!api->profile(3, &profile) && !profile.channels[0][0] && !allocations);

    fail_worker = true;
    assert(api->start() == SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !api->running());
    fail_worker = false;
    assert(!api->start() && api->running() && worker_creations == 2);
    assert(api->start() == SOLAR_OS_NATIVE_ERROR_BUSY && worker_creations == 2);
    assert(!api->configure(3, &(solar_os_native_irc_update_v1_t){.port = 7000}) && worker_notifications == 1);
    linger = true;
    assert(api->stop() == SOLAR_OS_NATIVE_ERROR_TIMEOUT && api->running() && worker_notifications == 2);
    assert(api->start() == SOLAR_OS_NATIVE_ERROR_BUSY);
    linger = false;
    assert(!api->stop() && !api->running() && worker_notifications == 3);
    assert(!api->start() && !api->stop() && !api->running());
    assert(!allocations);
    return 0;
}
