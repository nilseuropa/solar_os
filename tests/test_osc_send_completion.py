from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class OscSendCompletionTest(unittest.TestCase):
    def test_command_and_repeat_options(self):
        source = (ROOT / "src/apps/solar_os_shell.c").read_text()
        table = re.search(r"^static const char \* const osc_subcommands\[\] = .*?;", source, re.M | re.S).group()
        function = production_functions(source, ("shell_completion_emit_osc_send",))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_shell_completion.h"
typedef struct {const char *prefix;char values[256];} shell_completion_match_t;
static void shell_completion_emit(shell_completion_match_t *s,const char *value) {
    if(strncmp(value,s->prefix,strlen(s->prefix)))return;
    if(s->values[0])strcat(s->values,"|");strcat(s->values,value);
}
/* TABLE */
/* FUNCTION */
static void check(const char *line,bool handled,const char *expected) {
    char storage[20][128]={{0}};const char *tokens[20];size_t starts[20],count;bool trailing;
    assert(solar_os_shell_completion_parse(line,strlen(line),&storage[0][0],128,20,starts,&count,&trailing));
    size_t completed=trailing?count:count-1;
    shell_completion_match_t state={.prefix=trailing?"":storage[count-1]};
    for(size_t i=0;i<completed;i++)tokens[i]=storage[i];
    bool result=shell_completion_emit_osc_send(&state,tokens,completed);
    if(!result && completed==1 && !strcmp(tokens[0],"osc"))
        for(size_t i=0;i<sizeof(osc_subcommands)/sizeof(osc_subcommands[0]);i++)shell_completion_emit(&state,osc_subcommands[i]);
    if(result!=handled || strcmp(state.values,expected)){fprintf(stderr,"%s -> %s\n",line,state.values);assert(false);}
}
int main(void) {
    check("osc s",false,"send");
    check("osc send ",true,"");
    check("osc send bot:9000 ",true,"");
    check("osc send bot:9000 /cmd ",true,"");
    check("osc send bot:9000 /cmd 0 ",true,"--rate|--duration");
    check("osc send bot:9000 /cmd 0 -0.2 --",true,"--rate|--duration");
    check("osc send bot:9000 /cmd 0 --rate ",true,"");
    check("osc send bot:9000 /cmd 0 --rate 20 --",true,"--duration");
    check("osc send bot:9000 /cmd --duration 1 0 --",true,"--rate");
    check("osc send bot:9000 /cmd 0 --rate 20 --duration 1 --",true,"");
    check("osc send bot:9000 /cmd 0 --unknown ",true,"");
    check("osc bindings ",false,"");
    puts("OSC send completion: command, numeric positions and unused repeat options passed");
}
'''.replace("/* TABLE */", table).replace("/* FUNCTION */", function)
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp, "completion")
            subprocess.run([
                "cc", "-x", "c", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "src/shell"), "-",
                str(ROOT / "src/shell/solar_os_shell_completion.c"), "-o", str(binary),
            ], input=harness, text=True, check=True)
            subprocess.run([str(binary)], check=True)
