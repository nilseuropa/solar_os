from pathlib import Path
import json
import shlex
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT=Path(__file__).resolve().parents[1]


class ShellCompletionStackTest(unittest.TestCase):
    def test_target_completion_stack_frames(self):
        commands=ROOT/'.pio/build/solar_term/compile_commands.json'
        if not commands.is_file():self.skipTest('Configured SolarTerm target required')
        entry=next(c for c in json.loads(commands.read_text()) if Path(c['file']).name=='solar_os_shell.c')
        args=shlex.split(entry['command'])
        if not Path(args[0]).is_file():self.skipTest('ESP32-S3 compiler required')
        with tempfile.TemporaryDirectory() as temp:
            args[args.index('-o')+1]=str(Path(temp)/'shell.o')
            subprocess.run(args+['-fstack-usage'],cwd=entry['directory'],check=True)
            frames={row.rsplit('\t',2)[0].rsplit(':',1)[-1]:int(row.rsplit('\t',2)[1])
                    for row in (Path(temp)/'shell.su').read_text().splitlines()}
            # Preserve room for app discovery, filesystem calls and the caller
            # on the display's 8 KiB main task. The regression used 2688 bytes.
            self.assertLessEqual(frames['shell_complete_command'],1536)
            self.assertLessEqual(frames['shell_complete_argument_with_work'],256)
            self.assertLessEqual(frames['shell_complete_argument'],128)

    def test_completion_workspace_lifetime_and_low_memory(self):
        source=(ROOT/'src/apps/solar_os_shell.c').read_text()
        start=source.index('typedef struct {\n    const char *completed_tokens[SHELL_ARG_MAX];')
        end=source.index('} shell_argument_completion_work_t;',start)+len('} shell_argument_completion_work_t;')
        functions=production_functions(source,('shell_complete_argument_with_work','shell_complete_argument'))
        fixture=(ROOT/'tests/host/shell_completion_work_test.inc').read_text()
        code=fixture.replace('/* WORK TYPE */',source[start:end]).replace('/* FUNCTIONS */',functions)
        with tempfile.TemporaryDirectory() as temp:
            file=Path(temp)/'completion.c';file.write_text(code)
            binary=Path(temp)/'completion'
            subprocess.run(['cc','-std=c11','-Os','-g','-Wall','-Wextra','-Werror',
                '-Wno-format-truncation',  # Completion clamps output to the shell line capacity.
                '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                '-I'+str(ROOT/'src/shell'),str(file),str(ROOT/'src/shell/solar_os_shell_completion.c'),
                '-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)


if __name__=='__main__':unittest.main()
