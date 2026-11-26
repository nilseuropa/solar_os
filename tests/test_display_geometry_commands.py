from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions
from test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class DisplayGeometryCommandsTest(unittest.TestCase):
    def test_only_persistent_session_operations_write_nvs(self):
        source = r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "solar_os_native_terminal_abi.h"
static solar_os_terminal_profile_t runtime={.orientation_degrees=180,.scale=2};
static solar_os_terminal_profile_t stored={.orientation_degrees=180,.scale=2};
static unsigned writes;
static bool fail_apply, on_owner, stopping;
static bool solar_os_shutdown_requested(void) { return stopping; }
static esp_err_t solar_os_sessions_run_on_owner(esp_err_t (*fn)(void *), void *request) {
    assert(!on_owner);
    on_owner = true;
    esp_err_t err = fn(request);
    on_owner = false;
    return err;
}
typedef struct {
    const char *target;
    uint32_t field, value;
    bool save;
} session_display_preference_t;
static esp_err_t solar_os_display_get_terminal_profile(const char *n,solar_os_terminal_profile_t *p) {
    assert(on_owner && n);*p=runtime;return ESP_OK;
}
static esp_err_t session_set_display_terminal_profile_internal(const char *n,const solar_os_terminal_profile_t *p) {
    assert(on_owner && n);if(fail_apply)return ESP_ERR_INVALID_STATE;runtime=*p;return ESP_OK;
}
static esp_err_t solar_os_terminal_profile_save_scale(const char *n,uint8_t v) {(void)n;stored.scale=v;writes++;return ESP_OK;}
static esp_err_t solar_os_terminal_profile_save_display_orientation(const char *n,uint16_t v) {(void)n;stored.orientation_degrees=v;writes++;return ESP_OK;}
'''
        source += production_functions((ROOT / 'src/services/solar_os_sessions.c').read_text(),
            ('session_set_display_preference_on_owner', 'solar_os_sessions_set_display_preference',
             'solar_os_sessions_set_display_scale', 'solar_os_sessions_set_display_orientation'))
        source += r'''
int main(void) {
    assert(solar_os_sessions_set_display_scale("display0",3,false)==ESP_OK);
    assert(solar_os_sessions_set_display_orientation("display0",90,false)==ESP_OK);
    assert(runtime.scale==3 && runtime.orientation_degrees==90 && !writes);
    assert(stored.scale==2 && stored.orientation_degrees==180);
    assert(solar_os_sessions_set_display_scale("display0",1,true)==ESP_OK);
    assert(solar_os_sessions_set_display_orientation("display0",0,true)==ESP_OK);
    assert(stored.scale==1 && stored.orientation_degrees==0 && writes==2);
    fail_apply=true;
    assert(solar_os_sessions_set_display_orientation("display0",270,true)==ESP_ERR_INVALID_STATE);
    assert(solar_os_sessions_set_display_scale("display0",4,true)==ESP_ERR_INVALID_STATE);
    assert(writes==2 && stored.scale==1 && stored.orientation_degrees==0);
    fail_apply = false;
    assert(solar_os_sessions_set_display_preference("display0", SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_SCALE, 257, true)==ESP_ERR_INVALID_ARG);
    assert(solar_os_sessions_set_display_preference("display0", SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_ORIENTATION, 65536, true)==ESP_ERR_INVALID_ARG);
    assert(solar_os_sessions_set_display_preference("display0", SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_FONT, 1, true)==ESP_ERR_INVALID_ARG);
    stopping = true;
    assert(solar_os_sessions_set_display_preference("display0", SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_SCALE, 3, true)==ESP_ERR_INVALID_STATE);
    assert(writes == 2 && runtime.scale == 1 && runtime.orientation_degrees == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path, binary = Path(directory, 'persistence.c'), Path(directory, 'persistence')
            path.write_text(source)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'),
                str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_appearance_propagates_only_to_peers_and_preserves_save_error(self):
        harness = r'''
#include <assert.h>
#include "esp_err.h"
#include "solar_os_native_terminal_abi.h"
#define SOLAR_OS_SESSION_MAX 3
#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 16
struct solar_os_terminal { bool palette; };
typedef struct { bool used; solar_os_terminal_t *terminal; int group; } solar_os_session_entry_t;
static solar_os_terminal_t local, peer, separate;
static struct { solar_os_session_entry_t sessions[SOLAR_OS_SESSION_MAX]; } session_state = {{
    {true, &local, 1}, {true, &peer, 1}, {true, &separate, 2},
}};
static unsigned writes, profiles;
static bool on_owner, palette;
static bool solar_os_shutdown_requested(void) { return false; }
static esp_err_t solar_os_sessions_run_on_owner(esp_err_t (*fn)(void *), void *request) {
    assert(!on_owner);
    on_owner = true;
    esp_err_t err = fn(request);
    on_owner = false;
    return err;
}
static solar_os_session_entry_t *session_find_by_terminal(const solar_os_terminal_t *terminal) {
    assert(on_owner);
    for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; ++i)
        if (session_state.sessions[i].terminal == terminal) return &session_state.sessions[i];
    return NULL;
}
static bool session_terminal_setting_is_persistent(const solar_os_session_entry_t *owner) { return owner->group == 1; }
static bool session_uses_same_display(const solar_os_session_entry_t *a, const solar_os_session_entry_t *b)
{ return a->group == b->group; }
static void session_store_terminal_profile(const solar_os_session_entry_t *owner, const solar_os_terminal_t *terminal)
{ assert(on_owner && owner->terminal == terminal); ++profiles; }
static void session_set_gfx_palette_inverted(const solar_os_session_entry_t *owner, bool value)
{ assert(on_owner && owner->group == 1); palette = value; }
static bool session_terminal_target_name(const solar_os_session_entry_t *owner, const solar_os_terminal_t *terminal,
    char *name, size_t cap)
{ (void)owner; (void)terminal; (void)name; (void)cap; return false; }
static esp_err_t solar_os_sessions_set_display_preference(const char *name, uint32_t field, uint32_t value, bool save)
{ (void)name; (void)field; (void)value; (void)save; return ESP_FAIL; }
#define UNUSED_SETTER(name, type) \
    static esp_err_t name(solar_os_terminal_t *term, type value) \
    { (void)term; (void)value; return ESP_ERR_INVALID_ARG; }
UNUSED_SETTER(solar_os_terminal_set_font, solar_os_terminal_font_t)
UNUSED_SETTER(solar_os_terminal_set_font_transient, solar_os_terminal_font_t)
UNUSED_SETTER(solar_os_terminal_set_text_size, solar_os_terminal_text_size_t)
UNUSED_SETTER(solar_os_terminal_set_text_size_transient, solar_os_terminal_text_size_t)
UNUSED_SETTER(solar_os_terminal_set_status_bar_visible, bool)
UNUSED_SETTER(solar_os_terminal_set_status_bar_visible_transient, bool)
static esp_err_t solar_os_terminal_set_status_bar_preference(bool value) { (void)value; return ESP_OK; }
static esp_err_t solar_os_terminal_set_palette_preference(bool value)
{ assert(!on_owner && value); ++writes; return ESP_OK; }
static esp_err_t solar_os_terminal_set_palette_inverted_transient(solar_os_terminal_t *term, bool value)
{ assert(on_owner && term); term->palette = value; return ESP_OK; }
static esp_err_t solar_os_terminal_set_palette_inverted(solar_os_terminal_t *term, bool value)
{ ++writes; (void)solar_os_terminal_set_palette_inverted_transient(term, value); return ESP_FAIL; }
typedef struct { solar_os_terminal_t *terminal; uint32_t field, value; } session_terminal_preference_t;
'''
        harness += production_functions((ROOT / 'src/services/solar_os_sessions.c').read_text(),
            ('session_set_terminal_appearance', 'session_set_terminal_preference_on_owner',
             'solar_os_sessions_set_terminal_preference', 'solar_os_sessions_set_terminal_palette_inverted'))
        harness += r'''
int main(void) {
    assert(solar_os_sessions_set_terminal_palette_inverted(&local, true) == ESP_FAIL);
    assert(writes == 1 && profiles == 1 && local.palette && peer.palette && !separate.palette && palette);
    assert(solar_os_sessions_set_terminal_preference(&local, SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_PALETTE, 2)
           == ESP_ERR_INVALID_ARG);
    assert(writes == 1 && profiles == 1 && palette);
    assert(solar_os_sessions_set_terminal_palette_inverted(NULL, true) == ESP_OK);
    assert(writes == 2 && profiles == 1);
    return 0;
}
'''
        run_c(harness, '-fsanitize=address,undefined', '-no-pie')

    def test_display_is_temporary_and_setterm_is_persistent(self):
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "esp_err.h"
#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 20
#define SOLAR_OS_DISPLAY_PRIMARY_TARGET "display0"
typedef struct {char text[256];unsigned errors;} solar_os_shell_io_t;
typedef struct {unsigned orientation_degrees,scale;} solar_os_terminal_profile_t;
typedef struct {void *gfx;} solar_os_context_t;
static solar_os_terminal_profile_t profile={.scale=2};
static unsigned changes;static bool saved;static char target[20];
static bool parse_size_arg(const char *s,size_t min,size_t max,size_t *v) {
    char *end;unsigned long n=strtoul(s,&end,10);if(!s[0]||*end||n<min||n>max)return false;*v=n;return true;
}
static size_t strlcpy(char *out,const char *s,size_t n) {assert(strlen(s)<n);strcpy(out,s);return strlen(s);}
static void solar_os_shell_io_printf(solar_os_shell_io_t *io,const char *fmt,...) {
    va_list args;va_start(args,fmt);vsnprintf(io->text,sizeof(io->text),fmt,args);va_end(args);
}
static void solar_os_shell_io_writeln(solar_os_shell_io_t *io,const char *s) {(void)io;(void)s;}
static void solar_os_shell_diag_esp(solar_os_shell_io_t *io,const char *s,esp_err_t e,const char *a,const char *b) {
    (void)s;(void)e;(void)a;(void)b;io->errors++;
}
static void solar_os_shell_diag_invalid(solar_os_shell_io_t *io,const char *a,const char *b,const char *c,
    const char *d,const char *e,bool f) {(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;io->errors++;}
static esp_err_t solar_os_display_get_terminal_profile(const char *name,solar_os_terminal_profile_t *out) {
    if(!strcmp(name,"missing"))return ESP_ERR_NOT_FOUND;*out=profile;return ESP_OK;
}
static esp_err_t solar_os_sessions_set_display_orientation(const char *name,uint16_t value,bool save) {
    strcpy(target,name);profile.orientation_degrees=value;saved=save;changes++;return ESP_OK;
}
static esp_err_t solar_os_sessions_set_display_scale(const char *name,uint8_t value,bool save) {
    strcpy(target,name);profile.scale=value;saved=save;changes++;return ESP_OK;
}
static bool solar_os_gfx_display_target_name(void *gfx,char *out,size_t size) {
    (void)gfx;assert(size>5);strcpy(out,"lcd0");return true;
}
'''
        source += production_functions((ROOT / 'src/shell/solar_os_shell_commands.c').read_text(),
                                       ('display_cmd_geometry', 'setterm_geometry_alias'))
        source += r'''
int main(void) {
    solar_os_shell_io_t io={0};solar_os_context_t ctx={0};
    char *normal[]={"display","orientation","display0","90"};
    display_cmd_geometry(&io,4,normal,false);assert(changes==1 && !saved && profile.orientation_degrees==90);
    char *runtime_scale[]={"display","scale","display0","3"};
    display_cmd_geometry(&io,4,runtime_scale,false);assert(changes==2 && !saved && profile.scale==3);
    char *query[]={"display","orientation","display0"};
    display_cmd_geometry(&io,3,query,false);assert(changes==2 && strstr(io.text,"90"));
    char *alias[]={"setterm","orientation","180"};
    setterm_geometry_alias(&ctx,&io,3,alias,NULL);
    assert(changes==3 && saved && !strcmp(target,"display0") && profile.orientation_degrees==180);
    ctx.gfx=&ctx;char *scale[]={"setterm","scale","3"};
    setterm_geometry_alias(&ctx,&io,3,scale,NULL);assert(changes==4 && saved && !strcmp(target,"lcd0") && profile.scale==3);
    char *targeted_alias[]={"setterm","orientation","0"};
    setterm_geometry_alias(&ctx,&io,3,targeted_alias,"oled0");
    assert(changes==5 && saved && !strcmp(target,"oled0") && profile.orientation_degrees==0);
    char *bad[]={"display","orientation","display0","45"};
    display_cmd_geometry(&io,4,bad,false);assert(changes==5 && io.errors==1);
    bad[1]="scale";bad[3]="0";display_cmd_geometry(&io,4,bad,false);assert(changes==5 && io.errors==2);
    bad[2]="missing";display_cmd_geometry(&io,4,bad,false);assert(changes==5 && io.errors==3);
    return 0;
}
'''
        source = source.replace('if(!strcmp(name,"missing"))return ESP_ERR_NOT_FOUND;*out=profile;',
                                'if(!strcmp(name,"missing"))return ESP_ERR_NOT_FOUND;\n    *out=profile;')
        with tempfile.TemporaryDirectory() as directory:
            path, binary = Path(directory, 'geometry.c'), Path(directory, 'geometry')
            path.write_text(source)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-fno-pie', '-no-pie', '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'),
                str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
