from pathlib import Path
import unittest

from test_foreground_presenter import production_functions
from test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class PortShellLifecycleTest(unittest.TestCase):
    def test_boot_reservation_retires_after_cleanup_and_workers_can_restart(self):
        source = (ROOT / "src/services/solar_os_port_shell.c").read_text()
        functions = production_functions(source, (
            "port_shell_task", "port_shell_reserved_worker", "solar_os_port_shell_init"))
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#define PORT_SHELL_TASK_STACK 24576
#define PORT_SHELL_TASK_PRIORITY 2
#define SOLAR_OS_TASK_ROLE_SYSTEM 0
#define SOLAR_OS_TASK_ROLE_FOREGROUND 1
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define tskNO_AFFINITY -1
#define SOLAR_OS_LOGI(...) ((void)0)
typedef int BaseType_t;
typedef void (*TaskFunction_t)(void *);
typedef struct task { TaskFunction_t entry; void *arg; unsigned stack; } *TaskHandle_t;
typedef struct { bool used; } port_shell_state_t;
static int port_shells_lock, lock_depth;
#define portENTER_CRITICAL(lock) do { assert((lock)==&port_shells_lock); assert(!lock_depth); lock_depth++; } while (0)
#define portEXIT_CRITICAL(lock) do { assert((lock)==&port_shells_lock); assert(lock_depth==1); lock_depth--; } while (0)
static TaskHandle_t port_shell_reserved_task, current_task;
static port_shell_state_t *port_shell_reserved_state;
static bool port_shell_reserved_initializing, fail_creation;
static unsigned creates, deletes, runs, wakeups, stack_bytes;
static port_shell_state_t boot_shell;
static esp_err_t solar_os_port_shell_init(bool reserve_worker);

static BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t entry,
    const char *name, unsigned stack, void *arg, unsigned priority, TaskHandle_t *out,
    int core, int role)
{
    assert(!lock_depth && stack==PORT_SHELL_TASK_STACK && priority==2 && core==-1);
    assert(role==SOLAR_OS_TASK_ROLE_SYSTEM || role==SOLAR_OS_TASK_ROLE_FOREGROUND);
    assert(!strcmp(name,"port_shell_rsv") || !strcmp(name,"port_shell"));
    creates++;
    if (fail_creation) return 0;
    *out=malloc(sizeof(**out));assert(*out);
    **out=(struct task){.entry=entry,.arg=arg,.stack=stack};stack_bytes+=stack;
    return pdPASS;
}
static void solar_os_task_delete_internal(TaskHandle_t task)
{
    assert(!task && !lock_depth && current_task && !port_shell_reserved_task && !port_shell_reserved_state);
    stack_bytes-=current_task->stack;free(current_task);current_task=NULL;deletes++;
}
static uint32_t ulTaskNotifyTake(int clear, uint32_t wait)
{
    assert(clear==pdTRUE && wait==portMAX_DELAY && !lock_depth);
    assert(current_task==port_shell_reserved_task && !port_shell_reserved_state);
    /* A notification without an assigned shell must retain the reservation. */
    if (++wakeups%2==0) port_shell_reserved_state=&boot_shell;
    return 1;
}
static void port_shell_run(port_shell_state_t *state)
{
    assert(!lock_depth && state->used);runs++;
    if (port_shell_reserved_task) {
        assert(current_task==port_shell_reserved_task && state==port_shell_reserved_state);
        unsigned before=creates;
        assert(solar_os_port_shell_init(true)==ESP_OK && creates==before);
        /* Cleanup can make the slot reusable before this call returns. The
         * task must remain claimed until all session cleanup has completed. */
        state->used=false;
        assert(port_shell_reserved_state==state && port_shell_reserved_task==current_task);
    } else state->used=false;
}
''' + functions + r'''
int main(void)
{
    assert(solar_os_port_shell_init(false)==ESP_OK && !creates);
    fail_creation=true;
    assert(solar_os_port_shell_init(true)==ESP_ERR_NO_MEM);
    assert(!port_shell_reserved_initializing && !port_shell_reserved_task && !stack_bytes);
    fail_creation=false;port_shell_reserved_initializing=true;
    assert(solar_os_port_shell_init(true)==ESP_ERR_INVALID_STATE);
    port_shell_reserved_initializing=false;
    for (unsigned cycle=0;cycle<3;cycle++) {
        unsigned before=creates;
        assert(solar_os_port_shell_init(true)==ESP_OK && creates==before+1);
        assert(port_shell_reserved_task && stack_bytes==PORT_SHELL_TASK_STACK);
        assert(solar_os_port_shell_init(true)==ESP_OK && creates==before+1);
        boot_shell.used=true;current_task=port_shell_reserved_task;
        current_task->entry(current_task->arg);
        assert(!current_task && !boot_shell.used && !stack_bytes);
        assert(!port_shell_reserved_state && !port_shell_reserved_task && !lock_depth);
        /* Subsequent shells use the ordinary worker entry, which also retires. */
        for (unsigned reopen=0;reopen<3;reopen++) {
            port_shell_state_t next={.used=true};
            assert(solar_os_task_create_pinned_internal(port_shell_task,"port_shell",
                PORT_SHELL_TASK_STACK,&next,2,&current_task,-1,SOLAR_OS_TASK_ROLE_FOREGROUND)==pdPASS);
            current_task->entry(current_task->arg);
            assert(!next.used && !current_task && !stack_bytes && !port_shell_reserved_task);
        }
    }
    assert(wakeups==6 && runs==12 && deletes==12);
}
''', "-fsanitize=address,undefined", "-no-pie")
