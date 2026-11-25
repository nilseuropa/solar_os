#include "solar_os_native_handles.h"
#include "freertos/FreeRTOS.h"

/* Slots store host adapter state and destructors. An adapter retaining module
 * callbacks must also retain their code lease until quiescent cleanup succeeds.
 * The lock protects lifetimes, not concurrent access to object state. */
#define NATIVE_HANDLES_MAX 128U
typedef struct {
    uint32_t id, kind, references;
    bool closing;
    void *state;
    solar_os_native_handle_destroy_fn destroy;
} native_handle_slot_t;
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
int solar_os_native_handle_create(uint32_t kind, void *state,
    solar_os_native_handle_destroy_fn destroy, solar_os_native_handle_t *out)
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
        *slot=(native_handle_slot_t){.id=next_handle,.kind=kind,.state=state,.destroy=destroy};
        *out=next_handle;
    }
    portEXIT_CRITICAL(&handles_lock);
    return *out ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
}
void *solar_os_native_handle_acquire(solar_os_native_handle_t id, uint32_t kind)
{
    void *state=NULL;
    portENTER_CRITICAL(&handles_lock);
    native_handle_slot_t *slot=find_handle(id);
    if (slot && !slot->closing && slot->kind==kind && slot->references!=UINT32_MAX) {
        ++slot->references;state=slot->state;
    }
    portEXIT_CRITICAL(&handles_lock);
    return state;
}
void solar_os_native_handle_release(solar_os_native_handle_t id)
{
    portENTER_CRITICAL(&handles_lock);
    native_handle_slot_t *slot=find_handle(id);
    if (slot && slot->references) --slot->references;
    portEXIT_CRITICAL(&handles_lock);
}
int solar_os_native_handle_close_kind(solar_os_native_handle_t id, uint32_t kind)
{
    void *state=NULL;
    solar_os_native_handle_destroy_fn destroy=NULL;
    portENTER_CRITICAL(&handles_lock);
    native_handle_slot_t *slot=find_handle(id);
    int result=SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (slot && (!kind || slot->kind==kind)) {
        result=SOLAR_OS_NATIVE_ERROR_BUSY;
        if (!slot->closing && !slot->references) {
            slot->closing=true;state=slot->state;destroy=slot->destroy;
        }
    }
    portEXIT_CRITICAL(&handles_lock);
    if (!destroy) return result;
    result=destroy(state);
    portENTER_CRITICAL(&handles_lock);
    /* The occupied, closing slot cannot be removed/reused by another caller. */
    if (result==SOLAR_OS_NATIVE_OK) *slot=(native_handle_slot_t){0};
    else slot->closing=false;
    portEXIT_CRITICAL(&handles_lock);
    return result;
}
int solar_os_native_handle_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id,0); }
uint32_t solar_os_native_handle_count(void)
{
    uint32_t count=0;
    portENTER_CRITICAL(&handles_lock);
    for (uint32_t i=0; i<NATIVE_HANDLES_MAX; ++i) count+=handles[i].id!=0;
    portEXIT_CRITICAL(&handles_lock);
    return count;
}
