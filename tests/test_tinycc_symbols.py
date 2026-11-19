"""Private compiler/frontend symbols cannot interpose on firmware or each other."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('tinycc_isolate',
    ROOT / 'modules/experimental/tinycc/isolate.py')
isolation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(isolation)


class TinyccSymbolsTest(unittest.TestCase):
    def test_two_private_runtimes_coexist_with_host_libc(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'runtime.c'
            source.write_text('''
#include <stddef.h>
extern int host_callback(int);
static int calls;
void *malloc(size_t bytes) { (void)bytes; return &calls; }
void free(void *p) { if (p == &calls) calls++; }
int app_main(int argc, char **argv) {
    (void)argv; free(malloc(1)); return host_callback(calls + argc);
}
''')
            raw = root / 'raw.o'
            subprocess.run(['cc', '-fno-builtin', '-c', str(source), '-o', str(raw)], check=True)
            objects = []
            for prefix in ('solar_os_tinycc_', 'solar_os_cc_'):
                output = root / (prefix + '.o')
                entry = isolation.isolate('nm', 'objcopy', raw, output, prefix)
                self.assertEqual(entry, prefix + 'app_main')
                self.assertEqual(isolation.imports('nm', output), {'host_callback'})
                self.assertEqual(isolation.defined_symbols('nm', output),
                    {prefix + name for name in ('malloc', 'free', 'app_main')})
                objects.append(output)
            host = root / 'host.c'
            host.write_text('''
#include <assert.h>
#include <stdlib.h>
int solar_os_tinycc_app_main(int, char **);
int solar_os_cc_app_main(int, char **);
int host_callback(int value) { return value + 10; }
int main(void) {
    int *allocation = malloc(sizeof(*allocation)); assert(allocation);
    *allocation = 42;
    assert(solar_os_tinycc_app_main(1, 0) == 12);
    assert(solar_os_cc_app_main(1, 0) == 12);
    assert(solar_os_tinycc_app_main(1, 0) == 13);
    assert(*allocation == 42); free(allocation);
}
''')
            binary = root / 'host'
            subprocess.run(['cc', '-fno-builtin', str(host), *map(str, objects),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_missing_entry_and_invalid_namespace_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, output = Path(temporary) / 'input.o', Path(temporary) / 'output.o'
            subprocess.run(['cc', '-x', 'c', '-c', '-o', str(source), '-'],
                input='int example(void) { return 0; }', text=True, check=True)
            for prefix in ('', 'malloc_', 'solar_os_bad prefix_', 'solar_os_ok_'):
                with self.subTest(prefix=prefix), self.assertRaises(ValueError):
                    isolation.isolate('nm', 'objcopy', source, output, prefix)
            self.assertFalse(output.exists())
