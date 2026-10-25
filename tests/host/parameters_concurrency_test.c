#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "solar_os_parameters.h"

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static bool entered, finish, write_mode;
static float stored;

static void wait_for_release(void)
{
    pthread_mutex_lock(&mutex);
    entered = true;
    pthread_cond_broadcast(&condition);
    while (!finish) pthread_cond_wait(&condition, &mutex);
    pthread_mutex_unlock(&mutex);
}
static esp_err_t get(void *user, float *value)
{ (void)user; wait_for_release(); *value = stored; return ESP_OK; }
static esp_err_t set(void *user, float value)
{ (void)user; wait_for_release(); stored = value; return ESP_OK; }
static void *access_parameter(void *arg)
{
    (void)arg;
    float value;
    esp_err_t err = write_mode ? solar_os_parameter_set_owned("test.value", "worker", 5) :
        solar_os_parameter_get("test.value", &value);
    assert(err == ESP_OK);
    return NULL;
}
static pthread_t begin_access(bool write)
{
    pthread_mutex_lock(&mutex);
    entered = finish = false; write_mode = write;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, access_parameter, NULL) == 0);
    while (!entered) pthread_cond_wait(&condition, &mutex);
    pthread_mutex_unlock(&mutex);
    return thread;
}
static void finish_access(pthread_t thread)
{
    pthread_mutex_lock(&mutex); finish = true;
    pthread_cond_broadcast(&condition); pthread_mutex_unlock(&mutex);
    assert(pthread_join(thread, NULL) == 0);
}

int main(void)
{
    const solar_os_parameter_definition_t def = {.name = "value", .label = "Value", .unit = "",
        .minimum = 0, .maximum = 10, .get = get, .set = set};
    solar_os_parameter_registration_t registration = SOLAR_OS_PARAMETER_REGISTRATION_INIT;
    assert(solar_os_parameters_register("test", &def, 1, &registration) == ESP_OK);
    pthread_t thread = begin_access(false);
    assert(solar_os_parameters_unregister(&registration) == ESP_ERR_INVALID_STATE);
    assert(solar_os_parameter_claim("test.value", "worker") == ESP_ERR_INVALID_STATE);
    assert(solar_os_parameters_update_range(&registration, "value", 0, 20) == ESP_ERR_INVALID_STATE);
    finish_access(thread);
    assert(solar_os_parameter_claim("test.value", "worker") == ESP_OK);
    thread = begin_access(true);
    assert(solar_os_parameter_set("test.value", 2) == ESP_ERR_INVALID_STATE);
    assert(solar_os_parameter_release("test.value", "worker") == ESP_ERR_INVALID_STATE);
    assert(solar_os_parameters_unregister(&registration) == ESP_ERR_INVALID_STATE);
    finish_access(thread); assert(stored == 5);
    assert(solar_os_parameter_release("test.value", "worker") == ESP_OK);
    assert(solar_os_parameters_update_range(&registration, "value", 0, 20) == ESP_OK);
    assert(solar_os_parameter_claim("test.value", "worker") == ESP_OK);
    assert(solar_os_parameters_unregister(&registration) == ESP_OK);
    assert(solar_os_parameters_register("test", &def, 1, &registration) == ESP_OK);
    assert(solar_os_parameter_set_owned("test.value", "worker", 7) == ESP_ERR_NOT_FOUND);
    assert(solar_os_parameters_unregister(&registration) == ESP_OK);
    puts("parameter claims and concurrent callback lifecycle tests passed");
    return 0;
}
