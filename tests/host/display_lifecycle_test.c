/* Use the complete production adapter with its existing hardware fixture. */
#define main expansion_fixture_main
#include "ssd1685_expansion_test.c"
#undef main
#include <pthread.h>
#include <stdatomic.h>
atomic_uint registry_mutex_initializations;
static pthread_barrier_t start_attach;
static const solar_os_expansion_binding_t concurrent_bindings[] = {
    {.kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .target = "spi0"},
    {.kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .value = 10},
    {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dc", .value = 17},
    {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset", .value = 16},
    {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "busy", .value = 15},
    {.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "panel", .value = 1},
};
static void *attach_worker(void *name)
{
    pthread_barrier_wait(&start_attach);
    assert(solar_os_ssd1685_attach(name, concurrent_bindings, 6) == ESP_OK);
    return NULL;
}
int main(void)
{
    expansion_fixture_main();
    primary = false;
    for (unsigned pass = 0; pass < 100; ++pass) {
        assert(!pthread_barrier_init(&start_attach, NULL, 2));
        pthread_t a, b;
        assert(!pthread_create(&a, NULL, attach_worker, "one"));
        assert(!pthread_create(&b, NULL, attach_worker, "two"));
        assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
        assert(devices[0] && devices[1] && devices[0] != devices[1]);
        assert(strcmp(devices[0]->name, devices[1]->name) != 0);
        /* The fixture models one exported target; release the two driver
         * instances after all tasks join without consulting that snapshot. */
        release_device_slot(0);
        release_device_slot(1);
        pthread_barrier_destroy(&start_attach);
    }
}
