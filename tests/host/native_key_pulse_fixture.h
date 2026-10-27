#include <stdatomic.h>
#include "solar_os_native_app_abi.h"
#include "native_key_pulse_type.inc"

/* The input producer and clock are supplied by each integration fixture. */
static bool native_key_raw_read(solar_os_native_foreground_key_v1_t *key);
static void native_key_raw_reset(void);
static uint64_t native_key_clock(void);
typedef struct {
    foreground_key_pulse_t key_pulses[FOREGROUND_KEY_PULSE_MAX];
    atomic_bool input_reset;
    void *keys;
} foreground_state_t;
static foreground_state_t key_pulse_state;
static bool key_pulse_caller = true;
static foreground_state_t *foreground_current(void) { return &key_pulse_state; }
static bool foreground_caller(void) { return key_pulse_caller; }
static uint64_t foreground_time_us(void) { return native_key_clock(); }
static int xQueueReceive(void *queue, void *key, unsigned wait)
{ assert(queue == key_pulse_state.keys && wait == 0); return native_key_raw_read(key); }
static void xQueueReset(void *queue)
{ assert(queue == key_pulse_state.keys); native_key_raw_reset(); }
#define pdTRUE 1
#include "native_key_pulse_read.inc"
static void native_key_pulse_reset(void)
{
    memset(&key_pulse_state, 0, sizeof(key_pulse_state));
    atomic_init(&key_pulse_state.input_reset, false);
    key_pulse_state.keys = &key_pulse_state;
    key_pulse_caller = true;
}
