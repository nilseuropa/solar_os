from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class StreamingHandoffTest(unittest.TestCase):
    def test_both_runtimes_defer_paint_to_the_display_owner(self):
        fixture = r'''
#include <assert.h>
#include <stdbool.h>
typedef struct { bool graphics; } solar_os_context_t;
typedef struct { bool dirty; } solar_os_terminal_t;
static solar_os_terminal_t physical;
static struct { solar_os_terminal_t *session_terminal; } solua;
static solar_os_terminal_t *python_current_terminal(void) {return solua.session_terminal;}
static void solar_os_context_set_streaming_graphics_active(solar_os_context_t *ctx,bool active) {ctx->graphics=active;}
static void solar_os_terminal_invalidate_render(solar_os_terminal_t *terminal) {terminal->dirty=true;}
/* STREAMING_HANDOFF */
int main(void) {
    void (*handlers[])(solar_os_context_t *,bool,bool *)={python_apply_streaming_mode,solua_apply_streaming_mode};
    for(unsigned i=0;i<2;++i) {
        solar_os_context_t port={0};bool ack=false;physical.dirty=false;solua.session_terminal=&physical;
        handlers[i](&port,true,&ack);assert(ack&&port.graphics&&!physical.dirty);
        ack=false;handlers[i](&port,false,&ack);assert(ack&&!port.graphics&&physical.dirty);
        solua.session_terminal=0;ack=false;handlers[i](&port,false,&ack);assert(ack);
    }
}
'''
        functions = []
        for runtime, prefix in (("python", "python"), ("lua", "solua")):
            source = (ROOT / f"src/apps/solar_os_{runtime}.c").read_text()
            functions.append(production_functions(source, (f"{prefix}_apply_streaming_mode",)))
            self.assertEqual(source.count(f"{prefix}_apply_streaming_mode("), 3)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(fixture.replace("/* STREAMING_HANDOFF */", "\n".join(functions)))
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)
