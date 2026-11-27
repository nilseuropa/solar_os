/* Lifecycle tests which do not model a scheduler must fail on unexpected
 * deferred cleanup. Specific scheduling fixtures override this definition. */
#include <stdlib.h>
#include "freertos/task.h"
__attribute__((weak)) void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    abort();
}
