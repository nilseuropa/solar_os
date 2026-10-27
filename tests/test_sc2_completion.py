from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SHELL = (ROOT / "src/apps/solar_os_shell.c").read_text()


def function(declaration):
    start = SHELL.index(declaration)
    brace = SHELL.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (SHELL[end] == "{") - (SHELL[end] == "}")
        end += 1
    return SHELL[start:end]


class Sc2CompletionTest(unittest.TestCase):
    def test_sc2_tables_and_dynamic_candidates(self):
        tables_start = SHELL.index("#if SOLAR_OS_PACKAGE_SERVICE_SC2\nstatic const char * const sc2_subcommands")
        tables = SHELL[tables_start:SHELL.index("#if SOLAR_OS_PACKAGE_APP_EMAIL", tables_start)]
        rules_start = SHELL.index("#if SOLAR_OS_PACKAGE_SERVICE_SC2\n    SHELL_COMPLETION_STATIC(path_sc2,")
        rules = SHELL[rules_start:SHELL.index("#if SOLAR_OS_PACKAGE_APP_EMAIL", rules_start)]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_shell_completion.h"
#define SOLAR_OS_PACKAGE_SERVICE_SC2 1
#define SC2_DEVICE_ID_HEX_LEN 32
#define SOLAR_OS_SC2_ALIAS_NAME_MAX 16
#define SOLAR_OS_SC2_ALIAS_MAX 16
#define ESP_OK 0
#define SOLAR_OS_CONVERSATION_ROOM 1
#define SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY 4
#define SOLAR_OS_MEMORY_EXTERNAL_PREFERRED 0
#define solar_os_memory_calloc(n,s,c,t) calloc(n,s)
#define solar_os_memory_free free
#define SHELL_COMPLETION_ANY "*"
#define SHELL_ARRAY_COUNT(a) (sizeof(a)/sizeof(a[0]))
enum { SHELL_COMPLETION_SOURCE_NONE, SHELL_COMPLETION_SOURCE_SC2_PEERS,
       SHELL_COMPLETION_SOURCE_SC2_ROOMS, SHELL_COMPLETION_SOURCE_SC2_ALIAS_VALUES };
typedef struct { const char *const *path; size_t path_count; int source;
                 const char *const *values; size_t value_count; } shell_completion_rule_t;
#define SHELL_COMPLETION_STATIC(p,v) {.path=p,.path_count=SHELL_ARRAY_COUNT(p),.values=v,.value_count=SHELL_ARRAY_COUNT(v)}
static const char *const on_off_values[] = {"on", "off"};
typedef struct { const char *prefix; char values[32][80]; size_t count; } shell_completion_match_t;
typedef struct { int provider, kind; char provider_key[64], title[64]; } solar_os_messaging_conversation_t;
int solar_os_sc2_alias_get(size_t i, char *name, char *peer) {
    (void)peer; if (i) return -1; strcpy(name,"alice"); return ESP_OK;
}
bool solar_os_sc2_peer_next(const char *after, char *out) {
    const char *id="00112233445566778899aabbccddeeff";
    if (strcmp(after,id)>=0) return false; strcpy(out,id); return true;
}
size_t solar_os_messaging_conversation_snapshot(solar_os_messaging_conversation_t *out, size_t cap) {
    const solar_os_messaging_conversation_t rows[]={
        {7,1,"00112233445566778899aabbccddeeff","Team room"},
        {7,1,"ffeeddccbbaa99887766554433221100","room ffeeddcc"},
        {7,1,"pub:general","#general"},
        {8,1,"11112233445566778899aabbccddeeff","Other provider"}};
    assert(cap>=4); memcpy(out,rows,sizeof(rows)); return 4;
}
bool solar_os_messaging_provider_is_class(int provider, const char *name) { return provider==7 && !strcmp(name,"sc2"); }
void shell_completion_emit(shell_completion_match_t *state, const char *value) {
    if (strncmp(value,state->prefix,strlen(state->prefix))) return;
    assert(state->count<32); snprintf(state->values[state->count++],80,"%s",value);
}
/* TABLES */
static const shell_completion_rule_t rules[]={
/* RULES */
};
/* PATH_MATCH */
/* EMIT */
static shell_completion_match_t collect(const char *const *tokens,size_t count,const char *prefix) {
    shell_completion_match_t state={.prefix=prefix};
    for(size_t i=0;i<SHELL_ARRAY_COUNT(rules);i++) {
        if(!shell_completion_path_matches(&rules[i],tokens,count,NULL)) continue;
        for(size_t j=0;j<rules[i].value_count;j++) shell_completion_emit(&state,rules[i].values[j]);
        if(rules[i].source) shell_completion_emit_sc2(&state,
            rules[i].source==SHELL_COMPLETION_SOURCE_SC2_ROOMS,
            rules[i].source==SHELL_COMPLETION_SOURCE_SC2_ALIAS_VALUES);
    }
    return state;
}
static bool has(const shell_completion_match_t *state,const char *value) {
    for(size_t i=0;i<state->count;i++) if(!strcmp(state->values[i],value)) return true;
    return false;
}
int main(void) {
    const char *root[]={"sc2"},*room[]={"sc2","room"},*peer[]={"sc2","safety"};
    const char *alias[]={"sc2","alias"},*value[]={"sc2","alias","alice"};
    const char *invite[]={"sc2","room","invite"};
    shell_completion_match_t state=collect(root,1,"");
    assert(has(&state,"safety") && has(&state,"status") && has(&state,"alias"));
    assert(has(&state,"room")==!!SOLAR_OS_PACKAGE_JOB_SC2_SYNC);
    state=collect(room,2,""); assert(has(&state,"create")==!!SOLAR_OS_PACKAGE_JOB_SC2_SYNC);
    state=collect(peer,2,"0011"); assert(state.count==1 && has(&state,"00112233445566778899aabbccddeeff"));
    state=collect(alias,2,"al"); assert(state.count==1 && has(&state,"alice"));
    state=collect(value,3,"off"); assert(state.count==1 && has(&state,"off"));
    state=collect(invite,3,"Te");
    assert(has(&state,"Team room")==!!(SOLAR_OS_PACKAGE_JOB_SC2_SYNC && SOLAR_OS_PACKAGE_SERVICE_SC2_PROVIDER));
    state=collect(invite,3,""); assert(!has(&state,"Other provider") && !has(&state,"#general") && !has(&state,"room ffeeddcc"));
    char encoded[80]; assert(solar_os_shell_completion_encode_token("Team room",encoded,sizeof(encoded)));
    assert(!strcmp(encoded,"\"Team room\""));
    puts("SC2 command, room, peer, alias and package-gate completion tests: ok");
}
'''
        harness = harness.replace("/* TABLES */", tables).replace("/* RULES */", rules)
        harness = harness.replace("/* PATH_MATCH */", function("static bool shell_completion_path_matches("))
        harness = harness.replace("/* EMIT */", function("static void shell_completion_emit_sc2("))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(harness)
            for job, provider in ((0, 0), (1, 0), (1, 1)):
                result = subprocess.run([
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
                    f"-DSOLAR_OS_PACKAGE_JOB_SC2_SYNC={job}", f"-DSOLAR_OS_PACKAGE_SERVICE_SC2_PROVIDER={provider}",
                    "-I" + str(ROOT / "src/shell"), str(path / "test.c"),
                    str(ROOT / "src/shell/solar_os_shell_completion.c"), "-o", str(path / "test"),
                ], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
