from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions


ROOT = Path(__file__).resolve().parents[1]


class ScriptAppHandoffLifecycleTest(unittest.TestCase):
    def test_completed_callers_finish_only_after_child_returns(self):
        fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct { void *requested_app; bool exit_requested; } solar_os_context_t;
static struct { bool done, repl_exit_requested; int exit_code; } python_app;
static struct { bool task_done, repl_exit_requested; int exit_code; } solua;
static unsigned finishes;
static void python_return_to_shell(solar_os_context_t *ctx, int code, const char *msg) {
    assert(code == 0 && msg == NULL); ctx->exit_requested = true; finishes++;
}
static void solua_return_to_shell(solar_os_context_t *ctx, int code, const char *msg) {
    python_return_to_shell(ctx, code, msg);
}
/* HANDLERS */
int main(void) {
    void (*handlers[])(solar_os_context_t *) = {python_finish_handoff, solua_finish_handoff};
    for (unsigned i = 0; i < 2; ++i) {
        solar_os_context_t ctx = {0}; finishes = 0;
        python_app.done = solua.task_done = false;
        python_app.repl_exit_requested = solua.repl_exit_requested = true;
        handlers[i](&ctx); assert(finishes == 0); /* Worker still unwinding. */
        python_app.done = solua.task_done = true;
        ctx.requested_app = &ctx;
        handlers[i](&ctx); assert(finishes == 0); /* Preserve pending launch. */
        ctx.requested_app = NULL;
        handlers[i](&ctx); assert(finishes == 1 && ctx.exit_requested);
        handlers[i](&ctx); assert(finishes == 1); /* DONE already finished it. */
        ctx.exit_requested = false;
        python_app.repl_exit_requested = solua.repl_exit_requested = false;
        handlers[i](&ctx); assert(finishes == 1); /* Ordinary script completion. */
    }
}
'''
        handlers = []
        for runtime, prefix in (("python", "python"), ("lua", "solua")):
            source = (ROOT / f"src/apps/solar_os_{runtime}.c").read_text()
            handlers.append(production_functions(source, (f"{prefix}_finish_handoff",)))
            self.assertIn(f"{prefix}_drain_events(ctx);\n        {prefix}_finish_handoff(ctx);", source)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(fixture.replace("/* HANDLERS */", "\n".join(handlers)))
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)

    def test_registry_preserves_caller_until_child_returns(self):
        source = (ROOT / "src/apps/solar_os_app_registry.c").read_text()
        function = production_functions(source, ("app_registry_request_native_launch",))
        fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
#define SOLAR_OS_APP_ARG_MAX 4
#define SOLAR_OS_APP_ARG_LEN 128
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_SIZE 2
#define SOLAR_OS_LAUNCH_CHILD_RETURN 3
typedef int esp_err_t;
typedef struct { int policy; } solar_os_context_t;
typedef struct { const void *app; const char *name; size_t min_argc, max_argc; } solar_os_app_registry_entry_t;
static size_t strlcpy(char *dest, const char *src, size_t size) {
    size_t len = strlen(src); assert(len < size); memcpy(dest, src, len + 1); return len;
}
static esp_err_t solar_os_context_request_launch_ex(solar_os_context_t *ctx,
        const void *app, int argc, char **argv, int policy) {
    assert(app != NULL && argc == 2);
    assert(strcmp(argv[0], "gameboy") == 0);
    assert(strcmp(argv[1], "/sdcard/Games/GameBoy/test/game.gb") == 0);
    ctx->policy = policy; return 0;
}
/* REGISTRY */
int main(void) {
    solar_os_context_t ctx = {0};
    solar_os_app_registry_entry_t entry = {&ctx, "gameboy", 2, 2};
    const char *args[] = {"/sdcard/Games/GameBoy/test/game.gb"};
    assert(app_registry_request_native_launch(&ctx, &entry, 1, args) == 0);
    assert(ctx.policy == SOLAR_OS_LAUNCH_CHILD_RETURN);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(fixture.replace("/* REGISTRY */", function))
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
