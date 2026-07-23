#include "solar_os_queue.h"

#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"

#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
#define SOLAR_OS_FREERTOS_EXTERNAL_MEMORY 1
#else
#define SOLAR_OS_FREERTOS_EXTERNAL_MEMORY 0
#endif

QueueHandle_t solar_os_queue_create(UBaseType_t length, UBaseType_t item_size)
{
#if SOLAR_OS_FREERTOS_EXTERNAL_MEMORY
    return xQueueCreateWithCaps(length,
                                item_size,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return xQueueCreate(length, item_size);
#endif
}

void solar_os_queue_delete(QueueHandle_t queue)
{
    if (queue == NULL) {
        return;
    }
#if SOLAR_OS_FREERTOS_EXTERNAL_MEMORY
    vQueueDeleteWithCaps(queue);
#else
    vQueueDelete(queue);
#endif
}

QueueHandle_t solar_os_queue_create_internal(UBaseType_t length,
                                             UBaseType_t item_size)
{
    return xQueueCreate(length, item_size);
}

void solar_os_queue_delete_internal(QueueHandle_t queue)
{
    if (queue != NULL) {
        vQueueDelete(queue);
    }
}
