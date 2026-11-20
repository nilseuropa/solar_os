#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "py/runtime.h"

struct test_vm_state test_vm;
static bool cancelled;
static uint32_t clock_tick;
static unsigned scheduled, delivered;

void solar_os_micropython_interrupt_reset(void);
void solar_os_micropython_request_interrupt(void);
void solar_os_micropython_check_interrupt(void);
void solar_os_micropython_vm_hook(void);
bool solar_os_micropython_stop_requested(void) { return cancelled; }
uint32_t xTaskGetTickCount(void) { return clock_tick; }
void vTaskDelay(uint32_t ticks) { clock_tick += ticks; }
void mp_sched_keyboard_interrupt(void) {
    scheduled++;
    test_vm.mp_pending_exception = (void *)1;
}
void mp_handle_pending(bool raise) {
    assert(raise);
    if (test_vm.mp_pending_exception) {
        test_vm.mp_pending_exception = NULL;
        delivered++;
    }
}
static void new_vm(void) {
    test_vm.mp_pending_exception = NULL;
    scheduled = delivered = 0;
    solar_os_micropython_interrupt_reset();
}
int main(void) {
    new_vm();
    solar_os_micropython_vm_hook();
    assert(!scheduled);
    cancelled = true;
    solar_os_micropython_request_interrupt(); /* UI request. */
    solar_os_micropython_check_interrupt(); /* A sleeping script wakes. */
    assert(scheduled == 1 && delivered == 1);
    for (unsigned cleanup_step = 0; cleanup_step < 32; cleanup_step++) {
        solar_os_micropython_vm_hook();
        solar_os_micropython_check_interrupt();
    }
    assert(scheduled == 1 && delivered == 1); /* finally can complete. */
    clock_tick += 999;
    solar_os_micropython_check_interrupt();
    assert(scheduled == 1);
    clock_tick++;
    solar_os_micropython_check_interrupt();
    assert(scheduled == 2 && delivered == 2); /* Cancellation stays sticky. */

    new_vm();
    clock_tick = UINT32_MAX - 500;
    solar_os_micropython_vm_hook();
    mp_handle_pending(true);
    solar_os_micropython_vm_hook();
    clock_tick += 999; /* Tick wrap preserves the cleanup interval. */
    solar_os_micropython_check_interrupt();
    assert(scheduled == 1 && delivered == 1);
    clock_tick++;
    solar_os_micropython_check_interrupt();
    assert(scheduled == 2 && delivered == 2);

    new_vm();
    solar_os_micropython_request_interrupt();
    clock_tick += 5000; /* Native I/O has not delivered the exception yet. */
    solar_os_micropython_vm_hook();
    solar_os_micropython_check_interrupt();
    solar_os_micropython_check_interrupt();
    assert(scheduled == 1 && delivered == 1);
    cancelled = false;
    clock_tick += 5000;
    solar_os_micropython_vm_hook();
    assert(scheduled == 1);
    puts("MicroPython cancellation allows bounded cleanup without duplicate interrupts");
    return 0;
}
