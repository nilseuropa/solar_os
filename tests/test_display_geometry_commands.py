from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DisplayGeometryCommandsTest(unittest.TestCase):
    def test_display_commands_and_setterm_aliases_share_persistence_policy(self):
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
static esp_err_t solar_os_sessions_set_display_scale(const char *name,uint8_t value) {
    strcpy(target,name);profile.scale=value;saved=true;changes++;return ESP_OK;
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
    display_cmd_geometry(&io,4,normal);assert(changes==1 && saved && profile.orientation_degrees==90);
    char *motion[]={"display","orientation","display0","270","--temporary"};
    display_cmd_geometry(&io,5,motion);assert(changes==2 && !saved && profile.orientation_degrees==270);
    char *query[]={"display","orientation","display0"};
    display_cmd_geometry(&io,3,query);assert(changes==2 && strstr(io.text,"270"));
    char *alias[]={"setterm","orientation","180"};
    setterm_geometry_alias(&ctx,&io,3,alias,NULL);
    assert(changes==3 && saved && !strcmp(target,"display0") && profile.orientation_degrees==180);
    ctx.gfx=&ctx;char *scale[]={"setterm","scale","3"};
    setterm_geometry_alias(&ctx,&io,3,scale,NULL);assert(changes==4 && saved && !strcmp(target,"lcd0") && profile.scale==3);
    char *temporary_alias[]={"setterm","orientation","0","--temporary"};
    setterm_geometry_alias(&ctx,&io,4,temporary_alias,"oled0");
    assert(changes==5 && !saved && !strcmp(target,"oled0") && profile.orientation_degrees==0);
    char *bad[]={"display","orientation","display0","45"};
    display_cmd_geometry(&io,4,bad);assert(changes==5 && io.errors==1);
    bad[1]="scale";bad[3]="0";display_cmd_geometry(&io,4,bad);assert(changes==5 && io.errors==2);
    bad[2]="missing";display_cmd_geometry(&io,4,bad);assert(changes==5 && io.errors==3);
    return 0;
}
'''
        source = source.replace('if(!strcmp(name,"missing"))return ESP_ERR_NOT_FOUND;*out=profile;',
                                'if(!strcmp(name,"missing"))return ESP_ERR_NOT_FOUND;\n    *out=profile;')
        with tempfile.TemporaryDirectory() as directory:
            path, binary = Path(directory, 'geometry.c'), Path(directory, 'geometry')
            path.write_text(source)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'tests/host'), str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
