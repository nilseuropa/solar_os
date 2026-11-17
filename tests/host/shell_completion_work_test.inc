#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_shell_completion.h"
#define SHELL_INPUT_MAX 384
#define SHELL_ARG_MAX 16
#define SHELL_NOINLINE __attribute__((noinline))
#define SOLAR_OS_MEMORY_TRANSIENT 0
#define SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES 1
#define SHELL_COMPLETION_FLAG_DIRS_ONLY 1
typedef struct { char input[SHELL_INPUT_MAX]; bool history_browsing; int history_index; } solar_os_context_t;
typedef struct { char tokens[SHELL_ARG_MAX][SHELL_INPUT_MAX]; size_t starts[SHELL_ARG_MAX],count; bool trailing_space; } shell_completion_parse_t;
typedef struct { char match[SHELL_INPUT_MAX]; size_t count; } shell_completion_match_t;
typedef struct { unsigned flags; } shell_completion_rule_t;
static unsigned allocations, frees, live, path_calls, print_calls;
static bool fail_alloc, alias, delegate_daq, delegate_gesture, path_rule, rule_seen=true, speed;
static unsigned matches=1;
static const char *candidate="target";
static solar_os_context_t *shell_session(solar_os_context_t *ctx) { return ctx; }
static void *solar_os_memory_calloc(size_t n,size_t bytes,int cls,const char *tag)
{ (void)cls;(void)tag;if(fail_alloc)return NULL;void *p=calloc(n,bytes);assert(p);allocations++;live++;return p; }
static void solar_os_memory_free(void *p) { assert(p && live);live--;frees++;free(p); }
static size_t strlcpy(char *out,const char *text,size_t n)
{ size_t bytes=strlen(text);if(n)snprintf(out,n,"%s",text);return bytes; }
static void shell_replace_input(solar_os_context_t *ctx,const char *text) { strcpy(ctx->input,text); }
static bool shell_completion_expand_alias(const shell_completion_parse_t *parse,size_t count,
    char *out,size_t n,char **tokens,size_t *used)
{ (void)parse;(void)count;if(!alias)return false;assert(n==SHELL_INPUT_MAX);strcpy(out,"help");tokens[0]=out;*used=1;return true; }
static bool shell_alias_lookup_target_command(const char *command,char *out,size_t n)
{ (void)command;(void)out;(void)n;return false; }
static bool shell_complete_daq_argument(solar_os_context_t *ctx,const char *command,
    const shell_completion_parse_t *parse,size_t index,size_t start,bool show)
{ (void)ctx;(void)command;(void)parse;(void)index;(void)start;(void)show;return delegate_daq; }
static bool shell_complete_gesture_argument(solar_os_context_t *ctx,const char *command,
    const shell_completion_parse_t *parse,size_t index,size_t start,bool show)
{ (void)ctx;(void)command;(void)parse;(void)index;(void)start;(void)show;return delegate_gesture; }
static bool shell_completion_normalize_mac_arguments(const char **tokens,size_t *count)
{ (void)tokens;(void)count;return speed; }
static const shell_completion_rule_t *shell_completion_find_path_rule(const char *const *tokens,size_t count)
{ (void)tokens;(void)count;static const shell_completion_rule_t rule={SHELL_COMPLETION_FLAG_DIRS_ONLY};return path_rule?&rule:NULL; }
static bool shell_completion_collect_matches(solar_os_context_t *ctx,const char *const *tokens,
    size_t count,const char *prefix,bool show,shell_completion_match_t *state)
{ (void)ctx;(void)count;(void)prefix;assert(!show);if(alias)assert(!strcmp(tokens[0],"help"));state->count=matches;strcpy(state->match,candidate);return rule_seen; }
static void shell_complete_path(solar_os_context_t *ctx,size_t start,const char *prefix,bool dirs,bool show)
{ (void)ctx;(void)start;(void)prefix;(void)show;assert(dirs);path_calls++; }
static void shell_print_argument_completion_matches(solar_os_context_t *ctx,const char *const *tokens,
    size_t count,const char *prefix)
{ (void)ctx;(void)tokens;(void)count;(void)prefix;print_calls++; }
/* WORK TYPE */
/* FUNCTIONS */
int main(void)
{
    solar_os_context_t ctx={.input="help ta"};
    shell_completion_parse_t parse={.count=2};strcpy(parse.tokens[0],"help");strcpy(parse.tokens[1],"ta");
    assert(!shell_complete_argument(&ctx,NULL,1,5,false));assert(!allocations);
    assert(!shell_complete_argument(&ctx,&parse,SHELL_ARG_MAX,5,false));assert(!allocations);
    fail_alloc=true;assert(shell_complete_argument(&ctx,&parse,1,5,false));
    assert(!strcmp(ctx.input,"help ta") && !allocations);fail_alloc=false;
    for(unsigned repeat=0;repeat<128;repeat++) {
        strcpy(ctx.input,"help ta");ctx.history_browsing=true;
        assert(shell_complete_argument(&ctx,&parse,1,5,false));assert(!strcmp(ctx.input,"help target "));
        assert(!ctx.history_browsing && !live);
        alias=true;strcpy(ctx.input,"h ta");assert(shell_complete_argument(&ctx,&parse,1,2,false));
        assert(!strcmp(ctx.input,"h target ") && !live);alias=false;
        delegate_daq=true;assert(shell_complete_argument(&ctx,&parse,1,5,false));assert(!live);delegate_daq=false;
        delegate_gesture=true;assert(shell_complete_argument(&ctx,&parse,1,5,false));assert(!live);delegate_gesture=false;
        rule_seen=false;assert(!shell_complete_argument(&ctx,&parse,1,5,false));assert(!live);
        path_rule=true;assert(shell_complete_argument(&ctx,&parse,1,5,false));assert(!live);
        path_rule=false;speed=true;assert(shell_complete_argument(&ctx,&parse,1,5,false));assert(!live);speed=false;
        rule_seen=true;matches=0;assert(!shell_complete_argument(&ctx,&parse,1,5,false));assert(!live);
        path_rule=true;assert(shell_complete_argument(&ctx,&parse,1,5,false));assert(!live);path_rule=false;
        matches=2;strcpy(ctx.input,"help ta");assert(shell_complete_argument(&ctx,&parse,1,5,false));
        assert(!strcmp(ctx.input,"help target") && !live);
        assert(shell_complete_argument(&ctx,&parse,1,5,true));assert(!live);matches=1;
    }
    parse.count=3;parse.trailing_space=true;strcpy(parse.tokens[0],"sc2");strcpy(parse.tokens[1],"room");strcpy(parse.tokens[2],"invite");
    strcpy(ctx.input,"sc2 room invite ");candidate="a b";
    assert(shell_complete_argument(&ctx,&parse,3,16,false));assert(!live);
    assert(!strcmp(ctx.input,"sc2 room invite \"a b\" "));
    assert(allocations==frees && path_calls==256 && print_calls==128);
    puts("Completion workspace: allocation failure, aliases, delegation, fallback, encoding and cleanup passed");
}
