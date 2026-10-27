from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class NativeAppPathCompletionTest(unittest.TestCase):
    def test_installed_rom_opener_uses_shell_path_completion(self):
        source = (ROOT / "src/apps/solar_os_shell.c").read_text()
        selector = production_functions(source, ("shell_is_path_command",))
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#define SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES 1
typedef struct { const char *file_extensions; } solar_os_app_registry_entry_t;
static solar_os_app_registry_entry_t app = {".rom"};
static bool installed = true;
static const solar_os_app_registry_entry_t *solar_os_app_registry_find(const char *name) {
    return installed && !strcmp(name, "amiga") ? &app : NULL;
}
''' + selector + r'''
int main(void) {
    assert(shell_is_path_command("amiga"));
    assert(shell_is_path_command("load"));
    assert(shell_is_path_command("cd"));
    assert(!shell_is_path_command("unknown"));
    app.file_extensions = "";
    assert(!shell_is_path_command("amiga"));
    app.file_extensions = ".rom"; installed = false;
    assert(!shell_is_path_command("amiga"));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "completion"
            subprocess.run(["cc", "-x", "c", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-o", str(binary), "-"], input=code, text=True, check=True)
            subprocess.run([str(binary)], check=True)
