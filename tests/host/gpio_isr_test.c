#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

#include "solar_os_gpio_isr.h"
#include "freertos/task.h"

static atomic_uint installs;
static esp_err_t install_result;

void vTaskDelay(TickType_t ticks)
{
    assert(ticks != 0);
    const struct timespec pause = {.tv_nsec = 100000};
    nanosleep(&pause, NULL);
}

esp_err_t gpio_install_isr_service(int flags)
{
    assert(flags == 0);
    atomic_fetch_add(&installs, 1);
    vTaskDelay(1);
    return install_result;
}

static void *ensure_service(void *arg)
{
    (void)arg;
    assert(solar_os_gpio_isr_service_ensure() == ESP_OK);
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const int mode = atoi(argv[1]);
    if (mode == 1) {
        install_result = ESP_ERR_NO_MEM;
        assert(solar_os_gpio_isr_service_ensure() == ESP_ERR_NO_MEM);
    }
    install_result = mode == 2 ? ESP_ERR_INVALID_STATE : ESP_OK;
    pthread_t callers[16];
    for (unsigned i = 0; i < 16; i++) {
        assert(pthread_create(&callers[i], NULL, ensure_service, NULL) == 0);
    }
    for (unsigned i = 0; i < 16; i++) {
        assert(pthread_join(callers[i], NULL) == 0);
    }
    assert(atomic_load(&installs) == (mode == 1 ? 2U : 1U));
    assert(solar_os_gpio_isr_service_ensure() == ESP_OK);
    assert(atomic_load(&installs) == (mode == 1 ? 2U : 1U));
    return 0;
}
