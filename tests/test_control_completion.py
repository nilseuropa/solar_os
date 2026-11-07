from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class ControlCompletionTest(unittest.TestCase):
    def test_get_and_set_complete_live_parameter_paths(self):
        source = (ROOT / "src/apps/solar_os_shell.c").read_text()
        tables = "\n".join(re.findall(
            r"^static const char \* const (?:control_parameter_subcommands|path_control_parameter\w*)\[\] = .*?;",
            source, re.M | re.S))
        rules = "\n".join(re.findall(
            r"^    SHELL_COMPLETION_(?:STATIC|PARAMETERS)\(path_control_parameter[^;]*?\),",
            source, re.M))
        macros = []
        lines = source.splitlines()
        for name in ("SHELL_COMPLETION_STATIC", "SHELL_COMPLETION_PARAMETERS"):
            index = next(i for i, line in enumerate(lines) if line.startswith("#define " + name + "("))
            while True:
                macros.append(lines[index])
                if not lines[index].endswith("\\"):
                    break
                index += 1
        functions = production_functions(source, (
            "shell_completion_path_matches", "shell_completion_emit_parameters"))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_parameters.h"
#include "solar_os_shell_completion.h"
#define SHELL_ARRAY_COUNT(a) (sizeof(a)/sizeof(a[0]))
#define SHELL_COMPLETION_ANY "*"
enum { SHELL_COMPLETION_SOURCE_STATIC, SHELL_COMPLETION_SOURCE_PARAMETERS };
typedef struct { const char *const *path; size_t path_count;
    const char *const *values; size_t value_count; unsigned source; } shell_completion_rule_t;
typedef struct { const char *prefix; char values[512]; } shell_completion_match_t;
static const char *paths[]={"diff_drive.base.linear_velocity_limit", "diff_drive.base.command_timeout_ms", "synth.volume"};
static size_t parameter_count=3;
size_t solar_os_parameter_count(void) { return parameter_count; }
bool solar_os_parameter_get_info(size_t index, solar_os_parameter_info_t *info) {
    assert(index<parameter_count);strcpy(info->path,paths[index]);return true;
}
static void shell_completion_emit(shell_completion_match_t *state,const char *value) {
    if(strncmp(value,state->prefix,strlen(state->prefix)))return;
    if(state->values[0])strcat(state->values,"|");
    strcat(state->values,value);
}
/* MACROS */
/* TABLES */
static const shell_completion_rule_t rules[]={
/* RULES */
};
/* FUNCTIONS */
static void check(const char *input,const char *expected) {
    char storage[8][128]={{0}};const char *tokens[8];size_t starts[8],count;bool trailing;
    assert(solar_os_shell_completion_parse(input,strlen(input),&storage[0][0],sizeof(storage[0]),8,starts,&count,&trailing));
    size_t completed=trailing?count:count-1;
    shell_completion_match_t state={.prefix=trailing?"":storage[count-1]};
    for(size_t i=0;i<completed;++i)tokens[i]=storage[i];
    for(size_t i=0;i<SHELL_ARRAY_COUNT(rules);++i) {
        if(!shell_completion_path_matches(&rules[i],tokens,completed,NULL))continue;
        if(rules[i].source==SHELL_COMPLETION_SOURCE_PARAMETERS)shell_completion_emit_parameters(&state);
        else for(size_t j=0;j<rules[i].value_count;++j)shell_completion_emit(&state,rules[i].values[j]);
    }
    if(strcmp(state.values,expected)) {fprintf(stderr,"%s: %s\n",input,state.values);assert(false);}
}
int main(void) {
    check("control parameter ","get|set");
    check("control parameter g","get");
    check("control parameter get ","diff_drive.base.linear_velocity_limit|diff_drive.base.command_timeout_ms|synth.volume");
    check("control parameter set diff_drive.base.","diff_drive.base.linear_velocity_limit|diff_drive.base.command_timeout_ms");
    check("control parameter get diff_drive.base.l","diff_drive.base.linear_velocity_limit");
    check("control parameter set synth.","synth.volume");
    check("control parameter get diff_drive.base.linear_velocity_limit ","");
    check("control parameter set synth.volume ","");
    check("control parameter set synth.volume 1 ","");
    check("control parameter get missing","");
    parameter_count=0;
    check("control parameter get ","");check("control parameter set ","");
    parameter_count=1;paths[0]="encoder.new.ppr";
    check("control parameter get enc","encoder.new.ppr");
    puts("Control parameter completion: get/set, prefixes, value positions and live registration PASS");
}
'''
        for marker, text in (("MACROS", "\n".join(macros)), ("TABLES", tables),
                             ("RULES", rules), ("FUNCTIONS", functions)):
            harness = harness.replace("/* " + marker + " */", text)
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "control-completion"
            subprocess.run([
                "cc", "-x", "c", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "src/shell"), "-",
                str(ROOT / "src/shell/solar_os_shell_completion.c"), "-o", str(binary),
            ], input=harness, text=True, check=True)
            subprocess.run([str(binary)], check=True)
