#include <pthread.h>
#include "terminal_fixture.h"
#include "solar_os_native_shell_io.h"
#include "solar_os_native_shell_io_abi.h"
#include "solar_os_shell_io.h"

static char wire[8192];
static size_t wire_length;
static bool fail_write, short_write;
static esp_err_t read_status;
bool solar_os_port_handle_valid(const solar_os_port_handle_t *port)
{ return port && port->index==0 && port->token==1; }
esp_err_t solar_os_port_read(const solar_os_port_handle_t *port, uint8_t *bytes, size_t capacity,
                             uint32_t timeout_ms, size_t *read_count)
{
    assert(solar_os_port_handle_valid(port) && capacity > 0 && timeout_ms == 0);
    if (read_status != ESP_OK) return read_status;
    bytes[0] = 0xff;
    *read_count = 1;
    return ESP_OK;
}
size_t solar_os_input_read_chars(char *buffer, size_t capacity)
{
    assert(capacity > 0);
    buffer[0] = 'q';
    return 1;
}
esp_err_t solar_os_port_write(const solar_os_port_handle_t *port, const uint8_t *bytes, size_t size, size_t *written)
{
    assert(solar_os_port_handle_valid(port) && wire_length+size<sizeof(wire));
    if(fail_write) return ESP_FAIL;
    *written=short_write && size ? size-1 : size;
    memcpy(wire+wire_length,bytes,*written);wire_length+=*written;wire[wire_length]=0;return ESP_OK;
}
static const solar_os_native_shell_io_api_v1_t *api;
static void *foreign(void *unused) { (void)unused;assert(!api->current());return NULL; }
static int vformat(solar_os_shell_io_t *io, const char *format, ...)
{ va_list args;va_start(args,format);int result=api->vprintf(io,format,args);va_end(args);return result; }
static void reset_wire(void) { wire_length=0;wire[0]=0; }
int main(void)
{
    api=solar_os_native_shell_io_get_service("shell_io",1,8);
    assert(api && api->struct_size==sizeof(*api) && !api->current());
    assert(!solar_os_native_shell_io_get_service("shell_io",2,8));
    assert(!solar_os_native_shell_io_get_service("shell_io",1,sizeof(*api)+1));
    assert(!solar_os_native_shell_io_get_service("missing",1,8));
    solar_os_port_handle_t port={.index=0,.token=1};solar_os_shell_io_t io;
    solar_os_shell_io_init_port(&io,&port,10,8);
    solar_os_context_t ctx={.shell_io=&io};const solar_os_app_t app={.name="test"};
    solar_os_application_scope_t scope;solar_os_application_scope_enter(&scope,&app,&ctx);
    assert(api->current()==&io && api->kind(&io)==SOLAR_OS_SHELL_IO_KIND_PORT);
    solar_os_shell_io_t fallback;
    assert(solar_os_shell_io_with_fallback(&ctx, &fallback, true) == &io);
    pthread_t thread;assert(!pthread_create(&thread,NULL,foreign,NULL) && !pthread_join(thread,NULL));
    assert(api->current()==&io);
    uint8_t input[1];
    size_t count;
    assert(api->poll_input(&io, input, sizeof(input), &count) == SOLAR_OS_NATIVE_OK);
    assert(count == 1 && input[0] == 0xff);
    read_status = ESP_ERR_TIMEOUT;
    assert(api->poll_input(&io, input, sizeof(input), &count) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(count == 0);
    assert(api->poll_input(&io, NULL, sizeof(input), &count) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->poll_input(&io, NULL, 0, &count) == SOLAR_OS_NATIVE_OK && count == 0);
    assert(api->poll_input(&fallback, input, sizeof(input), &count) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    api->set_terminal_profile(&io,SOLAR_OS_SHELL_TERMINAL_PROFILE_ANSI);
    assert(api->is_cursor_addressable(&io) && api->cols(&io)==10 && api->rows(&io)==8);
    assert(!api->printf(&io,"%d %.1f %s\n",42,1.5,"A") && !strcmp(wire,"42 1.5 A\r\n"));
    assert(!vformat(&io,"%s %u","V",9) && strstr(wire,"V 9"));
    assert(!api->set_cursor(&io,0,0));reset_wire();
    assert(!api->table_printf(&io,"%s","abcdefghijklmnop") && !strcmp(wire,"abcdefghi"));
    assert(api->cursor_col(&io)==9);assert(!api->newline(&io));reset_wire();
    assert(!api->table_write(&io,"12345678\xc3\xa9") && !strcmp(wire,"12345678 "));
    assert(!api->newline(&io));reset_wire();
    assert(!api->table_printf_bold(&io,"%d",7));
    assert(strstr(wire,"\x1b[1m7") && strstr(wire,"\x1b[22m") && !io.bold);
    assert(!api->set_bold(&io,true));reset_wire();
    assert(!api->printf_bold(&io,"%s %d","bold",5) && io.bold && strstr(wire,"bold 5"));
    fail_write=true;assert(api->write(&io,"failure")==SOLAR_OS_NATIVE_ERROR_FAILED);
    fail_write=false;short_write=true;assert(api->write(&io,"partial")==SOLAR_OS_NATIVE_ERROR_FAILED);short_write=false;
    assert(api->write(NULL,"x")==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    solar_os_application_scope_leave(&scope);assert(!api->current());
    assert(api->poll_input(&io, input, sizeof(input), &count) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    solar_os_application_scope_enter(&scope,NULL,&ctx);assert(!api->current());solar_os_application_scope_leave(&scope);

    uint8_t buffer[52*52*8];u8g2_t u={0};
    u8g2_SetupDisplay(&u,display,u8x8_dummy_cb,u8x8_dummy_cb,u8x8_dummy_cb);
    u8g2_SetupBuffer(&u,buffer,52,u8g2_ll_hvline_vertical_top_lsb,U8G2_R0);
    solar_os_display_geometry_t geometry;solar_os_display_geometry_init(&geometry,412,412,false);u8g2_SetUserPtr(&u,&geometry);
    solar_os_terminal_t *terminal=calloc(1,sizeof(*terminal));assert(terminal);solar_os_terminal_init(terminal,&u);
    ctx.terminal = terminal;
    solar_os_context_set_shell_io(&ctx, NULL);
    solar_os_shell_io_init_terminal(&fallback, NULL);
    solar_os_context_set_shell_io(&ctx, &fallback);
    assert(solar_os_shell_io_with_fallback(&ctx, &io, false) == &fallback);
    assert(api->kind(&fallback) == SOLAR_OS_SHELL_IO_KIND_NONE);
    assert(solar_os_shell_io_with_fallback(&ctx, &io, true) == &io);
    solar_os_application_scope_enter(&scope, &app, &ctx);
    assert(api->current() == &io && api->terminal(api->current()) == terminal);
    assert(api->poll_input(&io, input, sizeof(input), &count) == SOLAR_OS_NATIVE_OK);
    assert(count == 1 && input[0] == 'q');
    solar_os_application_scope_leave(&scope);
    solar_os_context_set_shell_io(&ctx, NULL);
    assert(solar_os_shell_io_with_fallback(&ctx, &io, false) == &io);
    assert(solar_os_context_shell_io(&ctx) == &io);
    const unsigned before = allocations;
    assert(api->terminal(&io)==terminal && !api->printf(&io,"%d %s",14,"\xc3\xa9"));
    assert(terminal->lines[0][0]=='1' && terminal->lines[0][3]==0xe9);
    assert(!api->printf_bold(&io,"%s","B") && (terminal->bold[0][0]&(1U<<4)));
    assert(!api->flush(&io) && frames && allocations==before);
    solar_os_terminal_deinit(terminal);free(terminal);
    return 0;
}
