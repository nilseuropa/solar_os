#pragma once
#define SOLAR_TEST_CUSTOM_MEMORY
#include "terminal_fixture.h"
#include "solar_os_shell_io.h"
#include "solar_os_tui_widgets.h"

static unsigned live_allocations;
static unsigned fail_at=UINT_MAX;
static char wire[32768];
static size_t wire_length;
static solar_os_tui_t *attached_tui;
static unsigned attaches, detaches;
void *solar_os_memory_alloc(size_t size,solar_os_memory_class_t kind,const char *tag)
{
    assert(size && tag && kind < SOLAR_OS_MEMORY_CLASS_COUNT);
    if(allocations++==fail_at) return NULL;
    void *p=malloc(size);if(p)++live_allocations;return p;
}
void *solar_os_memory_calloc(size_t count,size_t size,solar_os_memory_class_t kind,const char *tag)
{ void *p=solar_os_memory_alloc(count*size,kind,tag);if(p)memset(p,0,count*size);return p; }
void solar_os_memory_free(void *p) { if(p){assert(live_allocations);--live_allocations;}free(p); }
bool solar_os_port_handle_valid(const solar_os_port_handle_t *port)
{ return port && port->index==0 && port->token==1; }
esp_err_t solar_os_port_write(const solar_os_port_handle_t *port,const uint8_t *bytes,size_t size,size_t *written)
{
    assert(solar_os_port_handle_valid(port) && wire_length+size<sizeof(wire));
    memcpy(wire+wire_length,bytes,size);wire_length+=size;wire[wire_length]=0;*written=size;return ESP_OK;
}
void solar_os_sessions_attach_tui(solar_os_shell_io_t *io,solar_os_tui_t *tui)
{ assert(io && tui);attached_tui=tui;++attaches; }
void solar_os_sessions_detach_tui(solar_os_shell_io_t *io,const solar_os_tui_t *tui)
{ assert(io && tui);if(attached_tui==tui){attached_tui=NULL;++detaches;} }
bool solar_os_shutdown_requested(void) { return false; }
const char *esp_err_to_name(esp_err_t error) { (void)error;return "test error"; }
