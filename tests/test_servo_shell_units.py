from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class ServoShellUnitsTest(unittest.TestCase):
    def test_shell_and_stream_use_identical_radians(self):
        source = (ROOT / "src/shell/solar_os_shell_servo.c").read_text()
        code = production_functions(source, ("parse_uint", "parse_angle", "usage", "print_info", "solar_os_shell_cmd_servo"))
        fixture = r'''
#define main pwm_provider_suite
#include "pwm_provider_test.c"
#undef main
#include "solar_os_native_motion.h"
#include "solar_os_native_motion_abi.h"
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
typedef struct {char text[4096];size_t used;} solar_os_shell_io_t;
typedef struct {solar_os_shell_io_t io;} solar_os_context_t;
static solar_os_shell_io_t *solar_os_shell_command_io(solar_os_context_t *ctx) {return &ctx->io;}
static void solar_os_shell_io_printf(solar_os_shell_io_t *io,const char *fmt,...) {
    char text[192];va_list args;va_start(args,fmt);int n=vsnprintf(text,sizeof(text),fmt,args);va_end(args);
    assert(n>=0 && n<(int)sizeof(text) && io->used+(size_t)n<sizeof(io->text));
    memcpy(io->text+io->used,text,(size_t)n+1);io->used+=(size_t)n;
}
#define solar_os_shell_io_table_printf solar_os_shell_io_printf
static void solar_os_shell_io_writeln(solar_os_shell_io_t *io,const char *s) {solar_os_shell_io_printf(io,"%s\n",s);}
static const char *solar_os_shell_error_text(esp_err_t err) {(void)err;return "error";}
/* PRODUCTION */
static const char *run(const char *text) {
    static solar_os_context_t ctx;memset(&ctx,0,sizeof(ctx));char line[256],*argv[16],*save;int argc=0;
    snprintf(line,sizeof(line),"%s",text);
    for(char *p=strtok_r(line," ",&save);p;p=strtok_r(NULL," ",&save)) {assert(argc<16);argv[argc++]=p;}
    solar_os_shell_cmd_servo(&ctx,argc,argv);return ctx.io.text;
}
int main(void) {
    fake_bank_t bank={.name="bank0"};assert(attach(&bank)==ESP_OK);
    assert(strstr(run("servo open arm bank0:0 1000 2000 -0.785398 0.785398"),"opened disabled"));
    assert(!bank.outputs[0].enabled);
    assert(!*run("servo set arm 0.5"));
    solar_os_servo_info_t info;assert(solar_os_servo_status("arm",&info)==ESP_OK);
    assert(fabsf(info.config.angle_min_rad+.785398f)<1e-7f && info.commanded_angle_rad==.5f);
    assert(bank.outputs[0].pulse_width_us==1818);
    const char *s=run("servo status arm");assert(strstr(s,"commanded 0.5 rad") && !strstr(s,"deg"));
    solar_os_stream_open_options_t options={.direction=SOLAR_OS_STREAM_DIRECTION_SINK};
    solar_os_stream_handle_t h=SOLAR_OS_STREAM_HANDLE_INIT;
    assert(solar_os_stream_open_ex("servo.arm","publisher",&options,&h)==ESP_OK);
    float angle=.5f;size_t written;
    assert(solar_os_stream_write(&h,&angle,sizeof(angle),0,&written)==ESP_OK && bank.outputs[0].pulse_width_us==1818);
    solar_os_stream_scalar_sample_t sample;assert(solar_os_stream_observe_scalar_sample("servo.arm",NULL,&sample)==ESP_OK && sample.value==.5f);
    assert(solar_os_stream_close_ex(&h)==ESP_OK);
    assert(!*run("servo pulse arm 1000"));
    assert(solar_os_servo_status("arm",&info)==ESP_OK && info.commanded_angle_rad==info.config.angle_min_rad);
    assert(strstr(run("servo set arm 1"),"servo: error"));
    assert(!*run("servo close arm"));
    const solar_os_native_motion_api_v1_t *api=solar_os_native_motion_get_service("motion",1,sizeof(*api));assert(api && api->servo_open);
    solar_os_native_servo_config_v1_t c={.provider="bank0",.output=0,.frequency_hz=50,
        .pulse_min_us=1000,.pulse_max_us=2000,.angle_min=-.785398f,.angle_max=.785398f};
    assert(api->servo_open("native-arm","robot",&c)==0);
    assert(solar_os_servo_status("native-arm",&info)==ESP_OK && info.config.angle_min_rad==c.angle_min && !info.stream_writable);
    assert(api->servo_set("native-arm","robot",.5f)==0 && bank.outputs[0].pulse_width_us==1818);
    assert(solar_os_servo_status("native-arm",&info)==ESP_OK && info.commanded_angle_rad==.5f);
    assert(api->servo_close("native-arm","robot")==0);
    assert(solar_os_pwm_unregister("bank0")==ESP_OK);
    puts("Servo shell: calibration, commands, status, streams and pulse readback use radians");
}
'''.replace("/* PRODUCTION */", code)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(fixture)
            command = ["cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                       "-DESP_ERR_NOT_ALLOWED=0x10b", "-DSOLAR_OS_PACKAGE_DRIVER_PWM_NATIVE=0", "-DSOLAR_OS_PACKAGE_SERVICE_SERVO=1",
                       "-DSOLAR_OS_PACKAGE_SERVICE_MOTOR=0", "-DSOLAR_OS_PACKAGE_SERVICE_ENCODER=0",
                       "-include", str(ROOT / "tests/host/compat.h")]
            for include in ("tests/host/pwm_stubs", "tests/host", "include", "src/services"):
                command += ["-I", str(ROOT / include)]
            command += [str(path / "test.c")]
            command += [str(ROOT / "src/services" / name) for name in
                        ("solar_os_pwm.c", "solar_os_servo.c", "solar_os_stream.c", "solar_os_native_motion.c")]
            command += [str(ROOT / "tests/host/motion_endpoint_clock.c"), "-pthread", "-lm", "-o", str(path / "test")]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(path / "test")], check=True)
