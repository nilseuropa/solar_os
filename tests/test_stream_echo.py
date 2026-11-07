from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class StreamEchoTest(unittest.TestCase):
    def test_command_with_real_stream_registry(self):
        source = (ROOT / "src/shell/solar_os_shell_stream_echo.c").read_text()
        source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
        fixture = (ROOT / "tests/host/stream_echo_test.inc").read_text().replace("/* COMMAND */", source)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(fixture)
            result = subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I" + str(ROOT / "tests/host"), "-I" + str(ROOT / "src/services"),
                str(path / "test.c"), str(ROOT / "src/services/solar_os_stream.c"),
                "-lm", "-o", str(path / "test")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(path / "test")], check=True)

    def test_multiple_stream_completion(self):
        source = (ROOT / "src/apps/solar_os_shell.c").read_text()
        collect = production_functions(source, ("shell_completion_collect_matches",))
        self.assertIn("if (shell_completion_emit_stream_write(state, tokens, token_count)) return true;", collect)
        functions = production_functions(source, (
            "shell_completion_emit_streams", "shell_completion_emit_stream_write"))
        harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_stream.h"
#include "solar_os_shell_completion.h"
typedef struct {const char *prefix;char values[512];} shell_completion_match_t;
static const solar_os_stream_info_t streams[]={
    {.id="mot.left",.type=SOLAR_OS_STREAM_TYPE_SCALAR,.direction=SOLAR_OS_STREAM_DIRECTION_SINK},
    {.id="diff_drive.base.cmd_linear",.type=SOLAR_OS_STREAM_TYPE_SCALAR,.direction=SOLAR_OS_STREAM_DIRECTION_DUPLEX},
    {.id="enc.left",.type=SOLAR_OS_STREAM_TYPE_SCALAR,.direction=SOLAR_OS_STREAM_DIRECTION_SOURCE},
    {.id="uart0",.type=SOLAR_OS_STREAM_TYPE_BYTES,.direction=SOLAR_OS_STREAM_DIRECTION_DUPLEX}};
size_t solar_os_stream_count(void) {return sizeof(streams)/sizeof(streams[0]);}
bool solar_os_stream_get(size_t index,solar_os_stream_info_t *info) {*info=streams[index];return true;}
static void shell_completion_emit(shell_completion_match_t *state,const char *value) {
    if(strncmp(value,state->prefix,strlen(state->prefix)))return;
    if(state->values[0])strcat(state->values,"|");
    strcat(state->values,value);
}
/* FUNCTIONS */
static void check(const char *line,bool handled,const char *expected) {
    char storage[20][128]={{0}};const char *tokens[20];size_t starts[20],count;bool trailing;
    assert(solar_os_shell_completion_parse(line,strlen(line),&storage[0][0],sizeof(storage[0]),20,starts,&count,&trailing));
    size_t completed=trailing?count:count-1;
    shell_completion_match_t state={.prefix=trailing?"":storage[count-1]};
    for(size_t i=0;i<completed;++i)tokens[i]=storage[i];
    bool result=shell_completion_emit_stream_write(&state,tokens,completed);
    if(result!=handled || strcmp(state.values,expected)) {fprintf(stderr,"%s: %s\n",line,state.values);assert(false);}
}
int main(void) {
    check("stream write ",true,"mot.left|diff_drive.base.cmd_linear|--rate|--duration");
    check("stream write diff",true,"diff_drive.base.cmd_linear");
    check("stream write mot.left ",true,"");
    check("stream write mot.left -",true,"");
    check("stream write mot.left .2 diff",true,"diff_drive.base.cmd_linear");
    check("stream write mot.left .2 --",true,"--rate|--duration");
    check("stream write mot.left .2 --rate ",true,"");
    check("stream write mot.left .2 --rate 20 ",true,"mot.left|diff_drive.base.cmd_linear|--duration");
    check("stream write --duration 2 mot.left .2 --",true,"--rate");
    check("stream write --duration 2 --rate 20 mot.left .2 diff",true,"diff_drive.base.cmd_linear");
    check("stream write mot.left .2 diff_drive.base.cmd_linear ",true,"");
    check("stream write --unknown ",true,"");
    check("stream echo ",false,"");
    puts("stream write completion: multiple pairs, scalar sinks, option positions and prefixes passed");
}
'''.replace("/* FUNCTIONS */", functions)
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "completion"
            subprocess.run([
                "cc", "-x", "c", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "src/shell"), "-",
                str(ROOT / "src/shell/solar_os_shell_completion.c"), "-o", str(binary),
            ], input=harness, text=True, check=True)
            subprocess.run([str(binary)], check=True)
