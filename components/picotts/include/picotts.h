#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PICOTTS_SAMPLE_FREQ_HZ 16000
#define PICOTTS_SAMPLE_BITS 16

typedef void (*picotts_output_fn)(int16_t *samples, unsigned count);
typedef void (*picotts_error_notify_fn)(void);
typedef void (*picotts_idle_notify_fn)(void);

/*
 * The resource buffers are read directly by PicoTTS. They must remain valid
 * and unmodified until picotts_shutdown() returns.
 */
bool picotts_init_resources(unsigned priority,
                            picotts_output_fn output,
                            int core,
                            const void *ta_data,
                            size_t ta_size,
                            const void *sg_data,
                            size_t sg_size);

void picotts_add(const char *text, unsigned length);
void picotts_shutdown(void);
void picotts_set_error_notify(picotts_error_notify_fn callback);
void picotts_set_idle_notify(picotts_idle_notify_fn callback);

#ifdef __cplusplus
}
#endif
