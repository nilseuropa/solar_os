from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DisplayControllerModesTest(unittest.TestCase):
    def test_mode_changes_wait_for_presentations_and_release_ownership(self):
        source = (ROOT / "src/services/solar_os_display.c").read_text()
        function = production_functions(source, ("solar_os_display_set_controller_mode",))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
typedef int esp_err_t;
enum {ESP_OK, ESP_FAIL, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE,
      ESP_ERR_NOT_FOUND, ESP_ERR_NOT_SUPPORTED};
typedef pthread_mutex_t *SemaphoreHandle_t;
typedef const char *(*solar_os_display_mode_getter_t)(const void *);
typedef esp_err_t (*solar_os_display_mode_setter_t)(void *, const char *);
typedef int solar_os_board_display_t;
typedef struct {
    bool relax_active;
    uint32_t generation, terminal_epoch;
    size_t refs;
    SemaphoreHandle_t present_mutex;
    solar_os_board_display_t *board_display;
    struct {
        void *controller_context;
        solar_os_display_mode_getter_t controller_mode;
        solar_os_display_mode_setter_t set_controller_mode;
    } target;
} display_target_slot_t;
static display_target_slot_t display_targets[1];
static pthread_mutex_t display_targets_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t present=PTHREAD_MUTEX_INITIALIZER;
static atomic_bool waiting, entered;
static _Thread_local bool owns_present;
static esp_err_t setter_result;
#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 32
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
#define portENTER_CRITICAL(lock) assert(!pthread_mutex_lock(lock))
#define portEXIT_CRITICAL(lock) assert(!pthread_mutex_unlock(lock))
static bool display_target_name_valid(const char *name, size_t limit)
{ return name && *name && strlen(name)<limit; }
static int display_find_slot_locked(const char *name)
{ return strcmp(name,"display0") ? -1 : 0; }
static int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned wait)
{
    assert(mutex==&present && wait==portMAX_DELAY && !owns_present);
    atomic_store(&waiting,true);
    assert(!pthread_mutex_lock(mutex)); owns_present=true; return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex)
{ assert(owns_present && mutex==&present); owns_present=false; assert(!pthread_mutex_unlock(mutex)); }
static void display_release_slot_ref(size_t index, uint32_t generation)
{
    assert(index==0 && display_targets[0].generation==generation && display_targets[0].refs==1);
    --display_targets[0].refs;
}
static const char *get_mode(const void *context)
{
    assert(context && owns_present && display_targets[0].refs==1);
    return "refresh=auto grayscale=4";
}
static esp_err_t set_mode(void *context, const char *mode)
{
    assert(context && mode && owns_present && display_targets[0].refs==1);
    atomic_store(&entered,true); return setter_result;
}
#if SOLAR_OS_BOARD_HAS_DISPLAY
static const char *solar_os_board_display_controller_mode(const solar_os_board_display_t *board)
{ return get_mode(board); }
static esp_err_t solar_os_board_display_set_controller_mode(solar_os_board_display_t *board, const char *mode)
{ return set_mode(board,mode); }
#endif
''' + function + r'''
static void *change_mode(void *unused)
{
    (void)unused;
    assert(solar_os_display_set_controller_mode("display0","grayscale=2")==ESP_OK);
    return NULL;
}
int main(void)
{
    display_targets[0]=(display_target_slot_t){.generation=1,.present_mutex=&present,
        .target={.controller_context=&present,.controller_mode=get_mode,.set_controller_mode=set_mode}};
    assert(!pthread_mutex_lock(&present));
    pthread_t thread; assert(!pthread_create(&thread,NULL,change_mode,NULL));
    while (!atomic_load(&waiting)) sched_yield();
    assert(!atomic_load(&entered)); /* A presentation still owns the image. */
    assert(!pthread_mutex_unlock(&present));
    assert(!pthread_join(thread,NULL));
    assert(atomic_load(&entered) && display_targets[0].refs==0 && display_targets[0].terminal_epoch==1);
    setter_result=ESP_FAIL;
    assert(solar_os_display_set_controller_mode("display0","grayscale=16")==ESP_FAIL);
    assert(display_targets[0].refs==0 && display_targets[0].terminal_epoch==1 && !owns_present);
    setter_result=ESP_OK;
    assert(solar_os_display_set_controller_mode("display0","spi-khz=20000")==ESP_OK);
    assert(display_targets[0].refs==0 && display_targets[0].terminal_epoch==1);
    display_targets[0].relax_active=true;
    assert(solar_os_display_set_controller_mode("display0","grayscale=2")==ESP_ERR_INVALID_STATE);
    display_targets[0].relax_active=false;
    display_targets[0].present_mutex=NULL;
    assert(solar_os_display_set_controller_mode("display0","grayscale=2")==ESP_ERR_INVALID_STATE);
    assert(display_targets[0].refs==0);
    display_targets[0].present_mutex=&present;
#if SOLAR_OS_BOARD_HAS_DISPLAY
    display_targets[0].target.controller_context=NULL;
    display_targets[0].target.controller_mode=NULL;
    display_targets[0].target.set_controller_mode=NULL;
    static solar_os_board_display_t board;
    display_targets[0].board_display=&board;
    assert(solar_os_display_set_controller_mode("display0","grayscale=2")==ESP_OK);
    assert(display_targets[0].refs==0 && display_targets[0].terminal_epoch==2);
#endif
    assert(solar_os_display_set_controller_mode("absent","grayscale=2")==ESP_ERR_NOT_FOUND);
    assert(solar_os_display_set_controller_mode("display0",NULL)==ESP_ERR_INVALID_ARG);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            for board in (0, 1):
                binary = Path(tmp) / f"controller-{board}"
                subprocess.run(["cc", "-x", "c", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                "-pthread", f"-DSOLAR_OS_BOARD_HAS_DISPLAY={board}",
                                "-o", str(binary), "-"], input=harness, text=True, check=True)
                subprocess.run([str(binary)], check=True, timeout=10)
