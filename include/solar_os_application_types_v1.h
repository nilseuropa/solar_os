#pragma once
/* Stable application values. Rendering objects are opaque borrowed references;
 * their implementation and the kernel context remain private. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct solar_os_terminal solar_os_terminal_t;
typedef struct solar_os_gfx solar_os_gfx_t;

#define SOLAR_OS_APP_FLAG_RESUMABLE (1U << 0)
/* Reuse a launching display shell's terminal instead of allocating a new one. */
#define SOLAR_OS_APP_FLAG_SHELL_INLINE (1U << 1)
/* Receive structured local key events instead of their legacy character form. */
#define SOLAR_OS_APP_FLAG_KEY_EVENTS (1U << 2)
/* Receive generic absolute or relative pointer events. */
#define SOLAR_OS_APP_FLAG_POINTER_EVENTS (1U << 3)
/* Receive normalized analog axis events. */
#define SOLAR_OS_APP_FLAG_AXIS_EVENTS (1U << 4)
/* Receive recognized touchless and touch gesture events. */
#define SOLAR_OS_APP_FLAG_GESTURE_EVENTS (1U << 5)
/* Retain the ordinary graphics snapshot even when resume also manages workers. */
#define SOLAR_OS_APP_FLAG_RESUME_SNAPSHOT (1U << 6)
/* Receive typed local text as UTF-8 instead of its legacy character form.
 * With KEY_EVENTS, text follows the key press or repeat that typed it. */
#define SOLAR_OS_APP_FLAG_TEXT_EVENTS (1U << 7)

typedef enum {
    SOLAR_OS_APP_CLASS_UNSPECIFIED = 0,
    /* Sequential shell-style output is preserved as a transcript. */
    SOLAR_OS_APP_CLASS_COMMAND,
    /* Cell-addressed screen state is private to the foreground session. */
    SOLAR_OS_APP_CLASS_TUI,
    /* Graphics framebuffer state is private to the foreground session. */
    SOLAR_OS_APP_CLASS_GUI,
} solar_os_app_class_t;

typedef enum {
    SOLAR_OS_LAUNCH_REPLACE,
    SOLAR_OS_LAUNCH_CHILD_RETURN,
} solar_os_launch_policy_t;

#define SOLAR_OS_APP_DISCOVERY_NAME_MAX 48
#define SOLAR_OS_APP_DISCOVERY_ID_MAX 32
#define SOLAR_OS_APP_DISCOVERY_TITLE_MAX 48
#define SOLAR_OS_APP_DISCOVERY_SUMMARY_MAX 128
#define SOLAR_OS_APP_DISCOVERY_RUNTIME_MAX 8


typedef enum {
    SOLAR_OS_APP_DISCOVERY_NATIVE,
    SOLAR_OS_APP_DISCOVERY_PLAYGROUND,
} solar_os_app_discovery_kind_t;

typedef struct {
    char name[SOLAR_OS_APP_DISCOVERY_NAME_MAX];
    char id[SOLAR_OS_APP_DISCOVERY_ID_MAX];
    char title[SOLAR_OS_APP_DISCOVERY_TITLE_MAX];
    char summary[SOLAR_OS_APP_DISCOVERY_SUMMARY_MAX];
    char runtime[SOLAR_OS_APP_DISCOVERY_RUNTIME_MAX];
    solar_os_app_discovery_kind_t kind;
} solar_os_app_discovery_info_t;
