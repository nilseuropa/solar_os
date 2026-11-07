from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class OscReceiveTest(unittest.TestCase):
    def test_shell_with_real_streams_and_service(self):
        source = (ROOT / "src/shell/solar_os_shell_osc.c").read_text()
        source = source.replace('#include "solar_os_shell_osc_send.inc"',
            (ROOT / "src/shell/solar_os_shell_osc_send.inc").read_text())
        source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
        common = (ROOT / "src/shell/solar_os_shell_common.c").read_text()
        source = production_functions(common, ("solar_os_shell_parse_size_arg",)) + "\n" + source
        fixture = (ROOT / "tests/host/osc_receive_shell_test.inc").read_text().replace("/* COMMAND */", source)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(fixture)
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I" + str(ROOT / "tests/host"), "-I" + str(ROOT / "src/services"),
                "-I" + str(ROOT / "include"), str(path / "test.c"),
                str(ROOT / "src/services/solar_os_osc.c"),
                str(ROOT / "src/services/solar_os_parameters.c"),
                str(ROOT / "src/services/solar_os_stream.c"), "-lm", "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)

    def test_receive_completion(self):
        source = (ROOT / "src/apps/solar_os_shell.c").read_text()
        tables = "\n".join(re.findall(
            r"^static const char \* const (?:osc_source_types|osc_receive_options|path_osc_bind_type|path_osc_bind_receive|path_osc_receive_options)\[\] = .*?;",
            source, re.M | re.S))
        rules = "\n".join(re.findall(
            r"^    SHELL_COMPLETION_(?:STATIC|SCALAR_SINKS)\((?:path_osc_bind_type|path_osc_bind_receive|path_osc_receive_options)[^\n]+",
            source, re.M))
        macros = []
        lines = source.splitlines()
        for name in ("SHELL_COMPLETION_STATIC", "SHELL_COMPLETION_SCALAR_SINKS"):
            index = next(i for i, line in enumerate(lines) if line.startswith("#define " + name + "("))
            while True:
                macros.append(lines[index])
                if not lines[index].endswith("\\"):
                    break
                index += 1
        harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_stream.h"
#include "solar_os_shell_completion.h"
#define SHELL_ARRAY_COUNT(a) (sizeof(a)/sizeof(a[0]))
#define SHELL_COMPLETION_ANY "*"
#define SHELL_COMPLETION_FLAG_SCALAR_STREAMS 1
#define SHELL_COMPLETION_FLAG_SCALAR_SINKS 2
#define SHELL_COMPLETION_SOURCE_STREAMS 1
typedef struct {const char *const *path;size_t path_count;const char *const *values;size_t value_count;unsigned source,flags;} shell_completion_rule_t;
typedef struct {const char *prefix;char values[512];} shell_completion_match_t;
static const solar_os_stream_info_t streams[]={
    {.id="drive.linear",.type=SOLAR_OS_STREAM_TYPE_SCALAR,.direction=SOLAR_OS_STREAM_DIRECTION_DUPLEX},
    {.id="encoder",.type=SOLAR_OS_STREAM_TYPE_SCALAR,.direction=SOLAR_OS_STREAM_DIRECTION_SOURCE},
    {.id="motor",.type=SOLAR_OS_STREAM_TYPE_SCALAR,.direction=SOLAR_OS_STREAM_DIRECTION_SINK},
    {.id="uart",.type=SOLAR_OS_STREAM_TYPE_BYTES,.direction=SOLAR_OS_STREAM_DIRECTION_DUPLEX}};
size_t solar_os_stream_count(void){return SHELL_ARRAY_COUNT(streams);}
bool solar_os_stream_get(size_t i,solar_os_stream_info_t *out){*out=streams[i];return true;}
static void shell_completion_emit(shell_completion_match_t *s,const char *value){
    if(strncmp(value,s->prefix,strlen(s->prefix)))return;
    if(s->values[0])strcat(s->values,"|");strcat(s->values,value);
}
/* MACROS */
/* TABLES */
static const shell_completion_rule_t rules[]={
/* RULES */
};
/* FUNCTIONS */
static void check(const char *line,const char *expected){
    char storage[20][128]={{0}};const char *tokens[20];size_t starts[20],count;bool trailing;
    assert(solar_os_shell_completion_parse(line,strlen(line),&storage[0][0],sizeof(storage[0]),20,starts,&count,&trailing));
    size_t completed=trailing?count:count-1;shell_completion_match_t state={.prefix=trailing?"":storage[count-1]};
    for(size_t i=0;i<completed;++i)tokens[i]=storage[i];
    for(size_t i=0;i<SHELL_ARRAY_COUNT(rules);++i){
        if(!shell_completion_path_matches(&rules[i],tokens,completed,NULL))continue;
        if(rules[i].source==SHELL_COMPLETION_SOURCE_STREAMS)shell_completion_emit_streams(&state,true,true);
        else for(size_t j=0;j<rules[i].value_count;++j)shell_completion_emit(&state,rules[i].values[j]);
    }
    if(strcmp(state.values,expected)){fprintf(stderr,"%s: %s\n",line,state.values);assert(false);}
}
int main(void){
    check("osc bind base ","stream|control|receive");
    check("osc bind base re","receive");
    check("osc bind base receive ","drive.linear|motor");
    check("osc bind base receive dr","drive.linear");
    check("osc bind base receive motor ","");
    check("osc bind base receive motor /cmd arg=","arg=0|arg=1|arg=2|arg=3|arg=4|arg=5|arg=6|arg=7");
    check("osc bind base receive motor /cmd arg=1 ","");
    puts("OSC receive completion: direction, sink filtering and argument index passed");
}
'''
        functions = production_functions(source, ("shell_completion_path_matches", "shell_completion_emit_streams"))
        for key, value in (("MACROS", "\n".join(macros)), ("TABLES", tables), ("RULES", rules), ("FUNCTIONS", functions)):
            harness = harness.replace("/* " + key + " */", value)
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "completion"
            subprocess.run([
                "cc", "-x", "c", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "src/shell"), "-",
                str(ROOT / "src/shell/solar_os_shell_completion.c"), "-o", str(binary),
            ], input=harness, text=True, check=True)
            subprocess.run([str(binary)], check=True)
