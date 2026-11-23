from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions
from test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class DisplayCanvasTest(unittest.TestCase):
    def test_runtime_command_validation_reset_and_no_persistence(self):
        commands = production_functions((ROOT / "src/shell/solar_os_shell_commands.c").read_text(),
                                       ("display_apply_canvas", "display_cmd_canvas"))
        run_c(r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "solar_os_display_geometry.h"
typedef struct { char name[16]; solar_os_display_rect_t panel,canvas; } solar_os_display_target_t;
typedef struct { unsigned scale; } solar_os_terminal_profile_t;
typedef struct { const char *name; solar_os_display_rect_t canvas; bool reset; } display_canvas_request_t;
typedef struct { char text[512]; unsigned errors; } solar_os_shell_io_t;
static solar_os_display_geometry_t geometry;
static unsigned mutations,reflows;
static bool owner,busy;
static bool solar_os_display_find_target(const char *name,solar_os_display_target_t *target) {
    if(strcmp(name,"display0")) return false;
    strcpy(target->name,name);target->panel=geometry.panel;target->canvas=geometry.canvas;return true;
}
static esp_err_t solar_os_display_get_terminal_profile(const char *name,solar_os_terminal_profile_t *profile) {
    (void)name;profile->scale=1;return ESP_OK;
}
static esp_err_t solar_os_display_set_canvas(const char *name,const solar_os_display_rect_t *canvas) {
    (void)name;assert(owner);if(busy)return ESP_ERR_INVALID_STATE;
    if(!solar_os_display_geometry_set_canvas(&geometry,canvas))return ESP_ERR_INVALID_ARG;
    mutations++;return ESP_OK;
}
static esp_err_t solar_os_sessions_set_display_terminal_profile(const char *name,const solar_os_terminal_profile_t *p) {
    (void)name;assert(owner && p->scale==1);reflows++;return ESP_OK;
}
static esp_err_t solar_os_sessions_run_on_owner(esp_err_t (*fn)(void *),void *ctx) {
    assert(!owner);owner=true;esp_err_t err=fn(ctx);owner=false;return err;
}
static bool parse_size_arg(const char *s,size_t lo,size_t hi,size_t *out) {
    char *end;unsigned long value=strtoul(s,&end,10);
    if(!*s||*s=='-'||*end||value<lo||value>hi)return false;
    *out=value;return true;
}
static void solar_os_shell_io_printf(solar_os_shell_io_t *io,const char *fmt,...) {
    va_list args;va_start(args,fmt);vsnprintf(io->text,sizeof(io->text),fmt,args);va_end(args);
}
static void solar_os_shell_diag_esp(solar_os_shell_io_t *io,const char *command,int error,void *a,void *b) {
    (void)command;(void)error;(void)a;(void)b;io->errors++;
}
''' + commands + r'''
int main(void) {
    solar_os_shell_io_t io={0};solar_os_display_geometry_init(&geometry,412,412,true);
    char *query[]={"display","canvas","display0"};display_cmd_canvas(&io,3,query);
    assert(!mutations && !reflows && strstr(io.text,"412x412") && strstr(io.text,"width=291"));
    char *custom[]={"display","canvas","display0","61","62","280","270"};
    display_cmd_canvas(&io,7,custom);assert(mutations==1 && reflows==1 && geometry.canvas.y==62);
    char *full[]={"display","canvas","display0","full"};display_cmd_canvas(&io,4,full);
    assert(mutations==2 && reflows==2 && geometry.canvas.width==412 && !geometry.canvas.x);
    char *reset[]={"display","canvas","display0","default"};display_cmd_canvas(&io,4,reset);
    assert(mutations==3 && reflows==3 && geometry.canvas.width==291);
    custom[3]="400";display_cmd_canvas(&io,7,custom);assert(mutations==3 && io.errors==1);
    custom[3]="-1";display_cmd_canvas(&io,7,custom);assert(mutations==3 && strstr(io.text,"usage:"));
    query[2]="missing";display_cmd_canvas(&io,3,query);assert(io.errors==2 && mutations==3);
    busy=true;display_cmd_canvas(&io,4,full);assert(io.errors==3 && mutations==3);
    busy=false;custom[3]="10";display_cmd_canvas(&io,7,custom);assert(geometry.canvas.x==10);
    solar_os_display_geometry_init(&geometry,412,412,true);assert(geometry.canvas.x==60 && geometry.canvas.width==291);
}
''', "-I", str(ROOT / "src/services"), "-I", str(ROOT / "components/u8g2/src/clib"),
              str(ROOT / "src/services/solar_os_display_geometry.c"), "-fsanitize=address,undefined", "-no-pie")

    def test_geometry_and_pixels_with_real_gfx_and_u8g2(self):
        component = ROOT / "components/u8g2"
        cmake = (component / "CMakeLists.txt").read_text().split("\n)", 1)[0]
        sources = [component / name for name in re.findall(r'"([^"]+\.c)"', cmake)]
        sources += sorted((ROOT / "fonts/build/u8g2").glob("u8g2_font_solar_os_default_*_tf.c"))
        sources += [ROOT / "src/services/solar_os_display_geometry.c"]
        with tempfile.TemporaryDirectory() as directory:
            decoder = Path(directory) / "utf8.c"
            decoder.write_text("#include <stdint.h>\n#include <stddef.h>\n" + production_functions(
                (ROOT / "src/services/solar_os_fonts.c").read_text(), ("solar_os_utf8_next",)))
            binary = Path(directory) / "canvas"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-ffunction-sections", "-fdata-sections",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "src"), "-idirafter", str(ROOT / "include"),
                "-I", str(component / "src/clib"),
                str(ROOT / "tests/host/display_canvas_test.c"), str(decoder), *map(str, sources),
                "-Wl,--gc-sections", "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
