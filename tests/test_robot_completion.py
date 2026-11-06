from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class RobotCompletionTest(unittest.TestCase):
    def test_model_commands_paths_frames_and_position_batches(self):
        source = (ROOT / "src/apps/solar_os_shell.c").read_text()
        names = ("robot_domains", "robot_model_subcommands", "path_robot",
                 "path_robot_model", "path_robot_model_load")
        tables = "\n".join(re.search(
            r"^static const char \* const " + name + r"\[\] = .*?;",
            source, re.M | re.S).group() for name in names)
        rules = "\n".join(re.findall(
            r"^    SHELL_COMPLETION_(?:STATIC\(path_robot,|STATIC\(path_robot_model,|PATH\(path_robot_model_load,)[^\n]*",
            source, re.M))
        self.assertEqual(len(rules.splitlines()), 3)
        macros = []
        lines = source.splitlines()
        for name in ("SHELL_COMPLETION_STATIC", "SHELL_COMPLETION_PATH"):
            index = next(i for i, line in enumerate(lines) if line.startswith("#define " + name + "("))
            while True:
                macros.append(lines[index])
                if not lines[index].endswith("\\"):
                    break
                index += 1
        collect = production_functions(source, ("shell_completion_collect_matches",))
        self.assertIn("if (shell_completion_emit_robot_model(state, tokens, token_count)) return true;", collect)
        helper = production_functions(source, ("shell_completion_path_matches", "shell_completion_emit_robot_model"))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_robot_model.h"
#include "solar_os_shell_completion.h"
#define ESP_OK 0
#define SHELL_ARRAY_COUNT(a) (sizeof(a)/sizeof(a[0]))
#define SHELL_COMPLETION_ANY "*"
#define SHELL_COMPLETION_FLAG_PATH 1U
#define SHELL_COMPLETION_FLAG_DIRS_ONLY 2U
typedef struct { const char *const *path; size_t path_count;
    const char *const *values; size_t value_count; unsigned flags; } shell_completion_rule_t;
/* MACROS */
typedef struct { const char *prefix; char values[32][64]; size_t count; } shell_completion_match_t;
static bool loaded=true;
static unsigned reads;
static const char *links[]={"base_link","left_wheel_link","right_wheel_link","caster_ball_link"};
static const char *joints[]={"base_joint","left_wheel_joint","right_wheel_joint"};
static int solar_os_robot_model_get_info(solar_os_robot_model_info_t *info) {
    ++reads;if(!loaded)return -1;
    *info=(solar_os_robot_model_info_t){.links=4,.joints=3};return ESP_OK;
}
static int solar_os_robot_model_get_link(uint32_t index,char *out,size_t bytes) {
    assert(index<4 && bytes>=strlen(links[index])+1);strcpy(out,links[index]);return ESP_OK;
}
static int solar_os_robot_model_get_joint(uint32_t index,solar_os_robot_model_joint_info_t *out) {
    assert(index<3);memset(out,0,sizeof(*out));strcpy(out->name,joints[index]);
    out->kind=index?SOLAR_OS_ROBOT_MODEL_CONTINUOUS:SOLAR_OS_ROBOT_MODEL_FIXED;return ESP_OK;
}
static void shell_completion_emit(shell_completion_match_t *state,const char *value) {
    if(strncmp(value,state->prefix,strlen(state->prefix)))return;
    assert(state->count<32);strcpy(state->values[state->count++],value);
}
/* TABLES */
static const shell_completion_rule_t rules[]={
/* RULES */
};
/* FUNCTIONS */
static void check(const char *input,bool expect_handled,bool expect_path,const char *expected) {
    char storage[16][128]={{0}},values[512]="";
    const char *tokens[16];size_t starts[16],count;bool trailing;
    assert(solar_os_shell_completion_parse(input,strlen(input),&storage[0][0],sizeof(storage[0]),16,starts,&count,&trailing));
    size_t completed=trailing?count:count-1;
    shell_completion_match_t state={.prefix=trailing?"":storage[count-1]};
    for(size_t i=0;i<completed;++i)tokens[i]=storage[i];
    bool path=false,handled=shell_completion_emit_robot_model(&state,tokens,completed);
    if(!handled)for(size_t i=0;i<SHELL_ARRAY_COUNT(rules);++i) {
        if(!shell_completion_path_matches(&rules[i],tokens,completed,NULL))continue;
        if(rules[i].flags & SHELL_COMPLETION_FLAG_PATH)path=true;
        else handled=true;
        for(size_t j=0;j<rules[i].value_count;++j)shell_completion_emit(&state,rules[i].values[j]);
    }
    for(size_t i=0;i<state.count;++i) {if(i)strcat(values,"|");strcat(values,state.values[i]);}
    if(handled!=expect_handled || path!=expect_path || strcmp(values,expected)) {
        fprintf(stderr,"%s: handled=%d path=%d values=%s\n",input,handled,path,values);assert(false);
    }
}
int main(void) {
    check("robot ",true,false,"joint|model|start|stop");
    check("robot diff",true,false,"");
    check("robot diff_drive ",false,false,"");
    check("robot mo",true,false,"model");
    check("robot model ",true,false,"load|unload|info|links|joints|metadata|set|transform|point|vector");
    check("robot model tr",true,false,"transform");
    check("robot model load ",false,true,"");
    check("robot model load \"/sdcard/My Robot/sol",false,true,"");
    check("robot model load file.urdf ",false,false,"");
    check("robot model set ",true,false,"left_wheel_joint|right_wheel_joint");
    check("robot model set le",true,false,"left_wheel_joint");
    reads=0;check("robot model set left_wheel_joint ",true,false,"");assert(!reads);
    check("robot model set left_wheel_joint 0 ",true,false,"right_wheel_joint");
    check("robot model set left_wheel_joint 0 ri",true,false,"right_wheel_joint");
    check("robot model set left_wheel_joint 0 right_wheel_joint 0 ",true,false,"");
    check("robot model set right_wheel_joint 0 left_wheel_joint ",true,false,"");
    check("robot model transform ba",true,false,"base_link");
    check("robot model transform base_link le",true,false,"left_wheel_link");
    check("robot model transform base_link left_wheel_link ",true,false,"");
    check("robot model point ",true,false,"base_link|left_wheel_link|right_wheel_link|caster_ball_link");
    check("robot model point base_link ri",true,false,"right_wheel_link");
    check("robot model point base_link right_wheel_link ",true,false,"");
    check("robot model vector base_link ca",true,false,"caster_ball_link");
    check("robot model vector base_link caster_ball_link 1 2 ",true,false,"");
    check("robot model metadata ",false,false,"");
    check("robot joint status ",false,false,"");
    check("other model set ",false,false,"");
    loaded=false;
    check("robot model set ",true,false,"");
    check("robot model transform ",true,false,"");
    check("robot model load so",false,true,"");
    puts("Robot model completion: commands, paths, both frames, position batches and unloaded model PASS");
}
'''
        harness = harness.replace("/* TABLES */", tables).replace("/* RULES */", rules)
        harness = harness.replace("/* MACROS */", "\n".join(macros))
        harness = harness.replace("/* FUNCTIONS */", helper)
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "robot-completion"
            subprocess.run([
                "cc", "-x", "c", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "include"), "-I", str(ROOT / "src/shell"), "-",
                str(ROOT / "src/shell/solar_os_shell_completion.c"), "-o", str(binary),
            ], input=harness, text=True, check=True)
            subprocess.run([str(binary)], check=True)
