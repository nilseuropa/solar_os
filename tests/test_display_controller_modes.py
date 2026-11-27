from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DisplayControllerModesTest(unittest.TestCase):
    def test_mode_changes_wait_for_presentations_and_release_ownership(self):
        source = (ROOT / "src/services/solar_os_display.c").read_text()
        state = re.search(r"^typedef struct \{[^}]*\} display_controller_call_t;", source, re.M).group()
        functions = production_functions(source, (
            "display_controller_begin", "display_copy_controller_text",
            "solar_os_display_get_controller_mode", "solar_os_display_set_controller_mode",
            "solar_os_display_set_high_refresh_owned", "solar_os_display_set_high_refresh_override",
        ))
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
      ESP_ERR_NOT_FOUND, ESP_ERR_NOT_SUPPORTED, ESP_ERR_INVALID_SIZE};
typedef pthread_mutex_t *SemaphoreHandle_t;
typedef const char *(*solar_os_display_mode_getter_t)(const void *);
typedef esp_err_t (*solar_os_display_mode_setter_t)(void *, const char *);
typedef int solar_os_board_display_t;
typedef struct {
    void *controller_context;
    solar_os_display_mode_getter_t controller_mode, controller_mode_values;
    solar_os_display_mode_setter_t set_controller_mode;
} solar_os_display_target_t;
typedef struct {
    bool relax_active;
    uint32_t generation, terminal_epoch;
    size_t refs;
    SemaphoreHandle_t present_mutex;
    solar_os_board_display_t *board_display;
    const void *refresh_owner;
    bool refresh_changing;
    solar_os_display_target_t target;
} display_target_slot_t;
static display_target_slot_t display_targets[1];
static pthread_mutex_t display_targets_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t present=PTHREAD_MUTEX_INITIALIZER;
static atomic_bool waiting, entered;
static _Thread_local bool owns_present;
static esp_err_t setter_result;
static char driver_text[32];
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
    portENTER_CRITICAL(&display_targets_lock);
    assert(index==0 && display_targets[0].generation==generation && display_targets[0].refs==1);
    --display_targets[0].refs;
    portEXIT_CRITICAL(&display_targets_lock);
}
static size_t strlcpy(char *out, const char *input, size_t capacity)
{
    size_t size = strlen(input);
    assert(capacity);
    size_t copied = size < capacity ? size : capacity - 1;
    memcpy(out, input, copied);
    out[copied] = 0;
    return size;
}
static const char *get_mode(const void *context)
{
    assert(context && owns_present && display_targets[0].refs==1);
    return strcpy(driver_text, "refresh=auto grayscale=4");
}
static const char *get_choices(const void *context)
{
    assert(context && owns_present && display_targets[0].refs == 1);
    return strcpy(driver_text, "grayscale=2 grayscale=4");
}
static esp_err_t set_mode(void *context, const char *mode)
{
    assert(context && mode && owns_present && display_targets[0].refs==1);
    atomic_store(&entered,true); return setter_result;
}
static const char legacy_refresh_owner;
#if SOLAR_OS_BOARD_HAS_DISPLAY
static esp_err_t refresh_error;
static unsigned refresh_changes;
static esp_err_t solar_os_board_display_set_high_refresh_override(
    solar_os_board_display_t *board, bool enabled, uint16_t rate)
{
    assert(board == display_targets[0].board_display && display_targets[0].refs == 1);
    assert(display_targets[0].refresh_changing);
    assert(enabled ? rate == 255 : rate == 0);
    ++refresh_changes;
    return refresh_error;
}
static const char *solar_os_board_display_controller_mode(const solar_os_board_display_t *board)
{ return get_mode(board); }
static const char *solar_os_board_display_controller_mode_values(const solar_os_board_display_t *board)
{ return get_choices(board); }
static esp_err_t solar_os_board_display_set_controller_mode(solar_os_board_display_t *board, const char *mode)
{ return set_mode(board,mode); }
#endif
''' + state + "\n" + functions + r'''
static void *change_mode(void *unused)
{
    (void)unused;
    assert(solar_os_display_set_controller_mode("display0","grayscale=2")==ESP_OK);
    return NULL;
}
int main(void)
{
    display_targets[0]=(display_target_slot_t){.generation=1,.present_mutex=&present,
        .target={.controller_context=&present,.controller_mode=get_mode,.set_controller_mode=set_mode,
                 .controller_mode_values=get_choices}};
    assert(!pthread_mutex_lock(&present));
    pthread_t thread; assert(!pthread_create(&thread,NULL,change_mode,NULL));
    while (!atomic_load(&waiting)) sched_yield();
    assert(!atomic_load(&entered)); /* A presentation still owns the image. */
    assert(!pthread_mutex_unlock(&present));
    assert(!pthread_join(thread,NULL));
    assert(atomic_load(&entered) && display_targets[0].refs==0 && display_targets[0].terminal_epoch==1);
    char current[32], choices[32];
    assert(solar_os_display_get_controller_mode("display0",current,sizeof(current),choices,sizeof(choices))==ESP_OK);
    assert(!strcmp(current,"refresh=auto grayscale=4") && !strcmp(choices,"grayscale=2 grayscale=4"));
    memset(driver_text,0,sizeof(driver_text));
    assert(current[0]=='r' && choices[0]=='g' && display_targets[0].refs==0 && !owns_present);
    assert(solar_os_display_get_controller_mode("display0",current,sizeof(current),choices,1)==ESP_ERR_INVALID_SIZE);
    assert(!current[0] && !choices[0] && display_targets[0].refs==0 && !owns_present);
    setter_result=ESP_FAIL;
    assert(solar_os_display_set_controller_mode("display0","grayscale=16")==ESP_FAIL);
    assert(display_targets[0].refs==0 && display_targets[0].terminal_epoch==1 && !owns_present);
    setter_result=ESP_OK;
    assert(solar_os_display_set_controller_mode("display0","spi-khz=20000")==ESP_OK);
    assert(display_targets[0].refs==0 && display_targets[0].terminal_epoch==1);
    display_targets[0].relax_active=true;
    assert(solar_os_display_set_controller_mode("display0","grayscale=2")==ESP_ERR_INVALID_STATE);
    assert(solar_os_display_get_controller_mode("display0",NULL,0,choices,sizeof(choices))==ESP_ERR_INVALID_STATE);
    assert(!choices[0] && display_targets[0].refs==0 && !owns_present);
    display_targets[0].relax_active=false;
    display_targets[0].present_mutex=NULL;
    assert(solar_os_display_set_controller_mode("display0","grayscale=2")==ESP_ERR_INVALID_STATE);
    assert(display_targets[0].refs==0);
    display_targets[0].present_mutex=&present;
    display_targets[0].target.controller_mode_values=NULL;
    assert(solar_os_display_get_controller_mode("display0",NULL,0,choices,sizeof(choices))==ESP_ERR_NOT_SUPPORTED);
    assert(!choices[0] && display_targets[0].refs==0 && !owns_present);
#if SOLAR_OS_BOARD_HAS_DISPLAY
    display_targets[0].target.controller_context=NULL;
    display_targets[0].target.controller_mode=NULL;
    display_targets[0].target.set_controller_mode=NULL;
    static solar_os_board_display_t board;
    display_targets[0].board_display=&board;
    assert(solar_os_display_get_controller_mode("display0",current,sizeof(current),NULL,0)==ESP_OK);
    assert(!strcmp(current,"refresh=auto grayscale=4"));
    assert(solar_os_display_set_controller_mode("display0","grayscale=2")==ESP_OK);
    assert(display_targets[0].refs==0 && display_targets[0].terminal_epoch==2);
    assert(!solar_os_display_set_high_refresh_override("display0", true, 255));
    static int owner, other;
    assert(solar_os_display_set_high_refresh_owned("display0", &owner, true, 255) == ESP_ERR_INVALID_STATE);
    assert(refresh_changes == 1);
    assert(!solar_os_display_set_high_refresh_override("display0", false, 0));
    assert(!solar_os_display_set_high_refresh_owned("display0", &owner, true, 255));
    assert(solar_os_display_set_high_refresh_override("display0", false, 0) == ESP_ERR_INVALID_STATE);
    assert(solar_os_display_set_high_refresh_owned("display0", &other, false, 0) == ESP_ERR_INVALID_STATE);
    assert(refresh_changes == 3);
    refresh_error = ESP_FAIL;
    assert(solar_os_display_set_high_refresh_owned("display0", &owner, false, 0) == ESP_FAIL);
    assert(display_targets[0].refresh_owner == &owner && display_targets[0].refs == 0);
    refresh_error = ESP_OK;
    assert(!solar_os_display_set_high_refresh_owned("display0", &owner, false, 0));
    assert(!display_targets[0].refresh_owner && !display_targets[0].refs);
#else
    assert(solar_os_display_set_high_refresh_override("display0", true, 255) == ESP_ERR_NOT_SUPPORTED);
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
