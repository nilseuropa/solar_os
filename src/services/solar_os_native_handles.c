#include "solar_os_native_handles.h"
#include "freertos/FreeRTOS.h"

/* Slots store host adapter state and destructors. An adapter retaining module
 * callbacks must retain their code lease until quiescent cleanup succeeds.
 * Shared borrows protect lifetimes. Exclusive borrows also serialize access
 * to state. Owners quiesce creation before releasing their handles. */
#define NATIVE_HANDLES_MAX 128U
#define NATIVE_HANDLE_CLOSING (UINT32_C(1) << 31)
#define NATIVE_HANDLE_EXCLUSIVE (UINT32_C(1) << 30)
#define NATIVE_HANDLE_REFERENCE_MAX (NATIVE_HANDLE_EXCLUSIVE - 1U)
typedef struct {
    uint32_t id, kind, reference_state;
    void *state;
    solar_os_native_handle_destroy_fn destroy;
    const void *owner;
} native_handle_slot_t;
/* Share the old reference/closing word, so ownership adds no target slot RAM. */
_Static_assert(sizeof(void *) != 4 || sizeof(native_handle_slot_t) == 24, "native handle slot size");
static native_handle_slot_t handles[NATIVE_HANDLES_MAX];
static uint32_t next_handle;
static portMUX_TYPE handles_lock = portMUX_INITIALIZER_UNLOCKED;

static native_handle_slot_t *find_handle(uint32_t id)
{
    if (!id) return NULL;
    for (uint32_t i=0; i<NATIVE_HANDLES_MAX; ++i)
        if (handles[i].id == id) return &handles[i];
    return NULL;
}
static int handle_create(uint32_t kind, void *state,
    solar_os_native_handle_destroy_fn destroy, const void *owner,
    uint32_t references, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out=0;
    if (!kind || !state || !destroy) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    portENTER_CRITICAL(&handles_lock);
    native_handle_slot_t *slot=NULL;
    for (uint32_t i=0; i<NATIVE_HANDLES_MAX; ++i)
        if (!handles[i].id) { slot=&handles[i]; break; }
    if (slot && next_handle != UINT32_MAX) {
        ++next_handle;
        *slot=(native_handle_slot_t){.id=next_handle,.kind=kind,.reference_state=references,.state=state,.destroy=destroy,.owner=owner};
        *out=next_handle;
    }
    portEXIT_CRITICAL(&handles_lock);
    return *out ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
}
int solar_os_native_handle_create(uint32_t kind, void *state,
    solar_os_native_handle_destroy_fn destroy, solar_os_native_handle_t *out)
{ return handle_create(kind, state, destroy, NULL, 0, out); }
int solar_os_native_handle_create_owned(uint32_t kind, void *state,
    solar_os_native_handle_destroy_fn destroy, const void *owner, solar_os_native_handle_t *out)
{
    if (!owner) {
        if (out) *out=0;
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    return handle_create(kind, state, destroy, owner, 0, out);
}
int solar_os_native_handle_create_exclusive(uint32_t kind, void *state,
    solar_os_native_handle_destroy_fn destroy, const void *owner, solar_os_native_handle_t *out)
{
    return handle_create(kind, state, destroy, owner, NATIVE_HANDLE_EXCLUSIVE, out);
}
static void *handle_acquire(solar_os_native_handle_t id, uint32_t kind, const void *owner)
{
    void *state=NULL;
    portENTER_CRITICAL(&handles_lock);
    native_handle_slot_t *slot=find_handle(id);
    if (slot && slot->kind==kind && (!owner || slot->owner==owner) &&
        slot->reference_state < NATIVE_HANDLE_REFERENCE_MAX) {
        ++slot->reference_state;state=slot->state;
    }
    portEXIT_CRITICAL(&handles_lock);
    return state;
}
void *solar_os_native_handle_acquire(solar_os_native_handle_t id, uint32_t kind)
{ return handle_acquire(id, kind, NULL); }
void *solar_os_native_handle_acquire_owned(solar_os_native_handle_t id, uint32_t kind, const void *owner)
{ return owner ? handle_acquire(id, kind, owner) : NULL; }

int solar_os_native_handle_acquire_exclusive(solar_os_native_handle_t id, uint32_t kind, void **state)
{
    if (!state) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *state = NULL;
    portENTER_CRITICAL(&handles_lock);
    native_handle_slot_t *slot = find_handle(id);
    int result = SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (slot && slot->kind == kind) {
        result = SOLAR_OS_NATIVE_ERROR_BUSY;
        if (!slot->reference_state) {
            slot->reference_state = NATIVE_HANDLE_EXCLUSIVE;
            *state = slot->state;
            result = SOLAR_OS_NATIVE_OK;
        }
    }
    portEXIT_CRITICAL(&handles_lock);
    return result;
}

void solar_os_native_handle_release(solar_os_native_handle_t id)
{
    portENTER_CRITICAL(&handles_lock);
    native_handle_slot_t *slot=find_handle(id);
    if (slot && slot->reference_state && slot->reference_state < NATIVE_HANDLE_EXCLUSIVE) --slot->reference_state;
    portEXIT_CRITICAL(&handles_lock);
}
void solar_os_native_handle_release_exclusive(solar_os_native_handle_t id)
{
    portENTER_CRITICAL(&handles_lock);
    native_handle_slot_t *slot = find_handle(id);
    if (slot && slot->reference_state == NATIVE_HANDLE_EXCLUSIVE) slot->reference_state = 0;
    portEXIT_CRITICAL(&handles_lock);
}
static int handle_close(solar_os_native_handle_t id, uint32_t kind, const void *owner,
                        bool exclusive)
{
    void *state=NULL;
    solar_os_native_handle_destroy_fn destroy=NULL;
    portENTER_CRITICAL(&handles_lock);
    native_handle_slot_t *slot=find_handle(id);
    int result=SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (slot && (!kind || slot->kind==kind) && (!owner || slot->owner==owner)) {
        result=SOLAR_OS_NATIVE_ERROR_BUSY;
        const uint32_t expected = exclusive ? NATIVE_HANDLE_EXCLUSIVE : 0;
        if (slot->reference_state == expected) {
            slot->reference_state=NATIVE_HANDLE_CLOSING;state=slot->state;destroy=slot->destroy;
        }
    }
    portEXIT_CRITICAL(&handles_lock);
    if (!destroy) return result;
    result=destroy(state);
    portENTER_CRITICAL(&handles_lock);
    /* The occupied, closing slot cannot be removed/reused by another caller. */
    if (result==SOLAR_OS_NATIVE_OK) *slot=(native_handle_slot_t){0};
    else slot->reference_state=0;
    portEXIT_CRITICAL(&handles_lock);
    return result;
}
int solar_os_native_handle_close_exclusive(solar_os_native_handle_t id, uint32_t kind)
{ return handle_close(id, kind, NULL, true); }
int solar_os_native_handle_close_kind(solar_os_native_handle_t id, uint32_t kind)
{ return handle_close(id, kind, NULL, false); }
int solar_os_native_handle_close_owned(solar_os_native_handle_t id, uint32_t kind, const void *owner)
{ return owner ? handle_close(id, kind, owner, false) : SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT; }
int solar_os_native_handle_close(solar_os_native_handle_t id)
{ return handle_close(id,0,NULL, false); }
int solar_os_native_handle_close_owner(const void *owner)
{
    if (!owner) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    for (;;) {
        solar_os_native_handle_t last=0;
        portENTER_CRITICAL(&handles_lock);
        for (uint32_t i=0; i<NATIVE_HANDLES_MAX; ++i)
            if (handles[i].owner==owner && handles[i].id>last) last=handles[i].id;
        portEXIT_CRITICAL(&handles_lock);
        if (!last) return SOLAR_OS_NATIVE_OK;
        const int result=handle_close(last,0,owner, false);
        if (result && result!=SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
    }
}
uint32_t solar_os_native_handle_count(void)
{
    uint32_t count=0;
    portENTER_CRITICAL(&handles_lock);
    for (uint32_t i=0; i<NATIVE_HANDLES_MAX; ++i) count+=handles[i].id!=0;
    portEXIT_CRITICAL(&handles_lock);
    return count;
}
