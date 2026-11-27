#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "solar_os_display_layout_types_v1.h"

#define SOLAR_OS_DISPLAY_TARGET_OWNER_MAX 32

typedef struct {
    bool used, owns_display_target;
    void *app, *terminal;
} solar_os_session_entry_t;
typedef struct {
    bool ready;
    void *u8g2, *base_rotation;
    char owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
} solar_os_display_target_t;

static struct {
    void *ctx, *shell_terminal, *default_gfx;
    solar_os_session_entry_t sessions[1], *foreground_session;
    char builtin_display_target[16], builtin_display_base_target[16];
} session_state;
static char shell, terminal, rotation;
static bool on_owner, created, logical_present, block_remove, stop_timeout;
static unsigned backing_refs, remove_calls, rebinds;
static solar_os_display_layout_info_t registered_layout;

static void *solar_os_shell_app(void) { return &shell; }

static esp_err_t solar_os_sessions_run_on_owner(esp_err_t (*operation)(void *), void *context)
{
    assert(!on_owner);
    on_owner = true;
    const esp_err_t result = operation(context);
    on_owner = false;
    return result;
}

static size_t strlcpy(char *dst, const char *src, size_t capacity)
{
    assert(strlen(src) < capacity);
    strcpy(dst, src);
    return strlen(src);
}

static esp_err_t solar_os_display_layout_create(const solar_os_display_layout_request_v1_t *request,
                                                 char *busy_owner, size_t capacity)
{
    assert(on_owner && !created);
    if (busy_owner) assert(capacity && !busy_owner[0]);
    if (request->member_count != 2) return ESP_ERR_INVALID_ARG;
    registered_layout = (solar_os_display_layout_info_t){.kind = request->kind, .axis = request->axis};
    strcpy(registered_layout.name, request->name);
    if (request->kind == SOLAR_OS_DISPLAY_LAYOUT_JOIN) {
        registered_layout.backing_count = 2;
        registered_layout.logical_count = 1;
        strcpy(registered_layout.backing[0], request->members[0]);
        strcpy(registered_layout.backing[1], request->members[1]);
        strcpy(registered_layout.logical[0], request->name);
    } else {
        registered_layout.backing_count = 1;
        registered_layout.logical_count = 2;
        strcpy(registered_layout.backing[0], request->name);
        strcpy(registered_layout.logical[0], request->members[0]);
        strcpy(registered_layout.logical[1], request->members[1]);
    }
    created = logical_present = true;
    return ESP_OK;
}

static esp_err_t solar_os_display_layout_find(solar_os_display_layout_kind_t kind, const char *name,
                                               solar_os_display_layout_info_t *out)
{
    assert(on_owner);
    if (!created || kind != registered_layout.kind || strcmp(registered_layout.name, name))
        return ESP_ERR_NOT_FOUND;
    *out = registered_layout;
    return ESP_OK;
}

static esp_err_t solar_os_display_layout_remove(solar_os_display_layout_kind_t kind, const char *name)
{
    assert(on_owner && created && kind == registered_layout.kind && !strcmp(registered_layout.name, name));
    ++remove_calls;
    if (block_remove) return ESP_ERR_INVALID_STATE;
    logical_present = false;
    if (stop_timeout) {
        assert(backing_refs == 1);
        return ESP_ERR_TIMEOUT;
    }
    created = false;
    return ESP_OK;
}

static bool solar_os_display_find_target(const char *name, solar_os_display_target_t *out)
{
    assert(on_owner);
    if (!strcmp(name, "display0")) {
        *out = (solar_os_display_target_t){true, &terminal, &rotation, "layout-owner"};
        return true;
    }
    /* A lookup during a failed removal may overwrite the snapshot used for the claim. */
    memset(out, 0, sizeof(*out));
    return logical_present && !strcmp(name, registered_layout.logical[0]);
}

static esp_err_t solar_os_display_claim(const char *name, const char *owner, char *busy_owner, size_t capacity)
{
    assert(on_owner && !strcmp(name, "display0") && !strcmp(owner, "layout-owner"));
    assert(!busy_owner && !capacity && !backing_refs);
    ++backing_refs;
    return ESP_OK;
}

static esp_err_t solar_os_display_release(const char *name, const char *owner)
{
    assert(on_owner && !strcmp(name, "display0") && !strcmp(owner, "layout-owner") && backing_refs == 1);
    --backing_refs;
    return ESP_OK;
}

static esp_err_t session_rebind_builtin_shell_display(const char *from, const char *to)
{
    assert(on_owner && !strcmp(from, session_state.builtin_display_target));
    if (!strcmp(to, "display0")) assert(backing_refs == 1 && !logical_present);
    else assert(created && logical_present && !strcmp(to, registered_layout.logical[0]));
    strcpy(session_state.builtin_display_target, to);
    ++rebinds;
    return ESP_OK;
}

/* FUNCTIONS */

int main(void)
{
    session_state.ctx = &shell;
    session_state.shell_terminal = &terminal;
    session_state.default_gfx = &terminal;
    session_state.sessions[0] = (solar_os_session_entry_t){.used = true, .app = &shell, .terminal = &terminal};
    session_state.foreground_session = &session_state.sessions[0];
    strcpy(session_state.builtin_display_target, "display0");
    strcpy(session_state.builtin_display_base_target, "display0");

    char busy[16] = "stale";
    assert(solar_os_sessions_create_display_layout(NULL, busy, sizeof(busy)) == ESP_ERR_INVALID_ARG);
    assert(!busy[0]);
    const char *const backing[] = {"display1", "display0"};
    const solar_os_display_layout_request_v1_t join = {
        .kind = SOLAR_OS_DISPLAY_LAYOUT_JOIN, .name = "joined", .member_count = 2, .members = backing,
    };
    session_state.foreground_session = NULL;
    assert(solar_os_sessions_create_display_layout(&join, busy, sizeof(busy)) == ESP_ERR_INVALID_STATE);
    assert(!created);
    session_state.foreground_session = &session_state.sessions[0];
    assert(solar_os_sessions_create_display_layout(&join, busy, sizeof(busy)) == ESP_OK);
    assert(!strcmp(session_state.builtin_display_target, "joined") && rebinds == 1 && !on_owner);

    session_state.foreground_session = NULL;
    assert(solar_os_sessions_remove_display_layout(join.kind, join.name) == ESP_ERR_INVALID_STATE);
    assert(!remove_calls && created);
    session_state.foreground_session = &session_state.sessions[0];
    block_remove = true;
    assert(solar_os_sessions_remove_display_layout(join.kind, join.name) == ESP_ERR_INVALID_STATE);
    assert(rebinds == 1 && logical_present && !backing_refs);

    block_remove = false;
    stop_timeout = true;
    assert(solar_os_sessions_remove_display_layout(join.kind, join.name) == ESP_ERR_TIMEOUT);
    assert(created && !logical_present && !backing_refs && rebinds == 2 && !on_owner);
    assert(!strcmp(session_state.builtin_display_target, "display0"));
    stop_timeout = false;
    assert(solar_os_sessions_remove_display_layout(join.kind, join.name) == ESP_OK && !created);

    const char *const panes[] = {"first", "second"};
    const solar_os_display_layout_request_v1_t split = {
        .kind = SOLAR_OS_DISPLAY_LAYOUT_SPLIT, .axis = SOLAR_OS_DISPLAY_LAYOUT_VERTICAL,
        .name = "display0", .member_count = 2, .members = panes,
    };
    assert(solar_os_sessions_create_display_layout(&split, NULL, 0) == ESP_OK);
    assert(!strcmp(session_state.builtin_display_target, "first") && rebinds == 3);
    assert(solar_os_sessions_remove_display_layout(split.kind, split.name) == ESP_OK);
    assert(!strcmp(session_state.builtin_display_target, "display0") && rebinds == 4 && !backing_refs);
    assert(solar_os_sessions_remove_display_layout(split.kind, split.name) == ESP_ERR_NOT_FOUND);
}
