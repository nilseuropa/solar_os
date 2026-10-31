#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_parameters.h"

atomic_uint registry_mutex_initializations;
static pthread_barrier_t entered, release_callback;
static float *state;
static bool normalized_write;

size_t strlcpy(char *dst, const char *src, size_t capacity)
{
    size_t len = strlen(src);
    if (capacity) {
        size_t count = len < capacity - 1 ? len : capacity - 1;
        memcpy(dst, src, count);
        dst[count] = 0;
    }
    return len;
}

static esp_err_t get(void *user, float *value)
{
    pthread_barrier_wait(&entered);
    pthread_barrier_wait(&release_callback);
    *value = *(float *)user;
    return ESP_OK;
}

static esp_err_t set(void *user, float value)
{
    pthread_barrier_wait(&entered);
    pthread_barrier_wait(&release_callback);
    *(float *)user = value;
    return ESP_OK;
}

static void *reader(void *unused)
{
    (void)unused;
    if (normalized_write) {
        assert(solar_os_parameter_set_normalized("review.value", UINT16_MAX) == ESP_OK);
    } else {
        float value = 0;
        assert(solar_os_parameter_get("review.value", &value) == ESP_OK && value == 1);
    }
    return NULL;
}

int main(void)
{
    assert(!pthread_barrier_init(&entered, NULL, 2));
    assert(!pthread_barrier_init(&release_callback, NULL, 2));
    for (unsigned pass = 0; pass < 2; ++pass) {
        state = malloc(sizeof(*state));
        assert(state);
        *state = 1;
        normalized_write = pass != 0;
        solar_os_parameter_definition_t definition = {
            .name = "value", .label = "Value", .unit = "", .minimum = 0,
            .maximum = pass ? 20 : 10, .get = get, .set = set, .user = state,
        };
        solar_os_parameter_registration_t registration = SOLAR_OS_PARAMETER_REGISTRATION_INIT;
        assert(solar_os_parameters_register("review", &definition, 1, &registration) == ESP_OK);
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, reader, NULL));
        pthread_barrier_wait(&entered);
        assert(solar_os_parameters_unregister(&registration) == ESP_ERR_INVALID_STATE);
        assert(solar_os_parameter_count() == 0);
        solar_os_parameter_info_t info;
        assert(solar_os_parameter_find("review.value", &info) == ESP_ERR_NOT_FOUND);
        solar_os_parameter_registration_t replacement = SOLAR_OS_PARAMETER_REGISTRATION_INIT;
        assert(solar_os_parameters_register("review", &definition, 1, &replacement) == ESP_ERR_INVALID_STATE);
        pthread_barrier_wait(&release_callback);
        assert(!pthread_join(thread, NULL));
        assert(*state == (pass ? 20 : 1));
        assert(solar_os_parameters_unregister(&registration) == ESP_OK);
        free(state);
    }
    pthread_barrier_destroy(&entered);
    pthread_barrier_destroy(&release_callback);
}
