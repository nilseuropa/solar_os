from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions
from host_cjson import CJSON

ROOT = Path(__file__).resolve().parents[1]


class PackageUpgradeTest(unittest.TestCase):
    def compile_run(self, code, extra_sources=(), extra_flags=()):
        with tempfile.TemporaryDirectory() as tmp:
            cfile, binary = Path(tmp, 'upgrade.c'), Path(tmp, 'upgrade')
            cfile.write_text(code)
            subprocess.run([
                'cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-g', '-O1', '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', '-I' + str(CJSON),
                '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'tests/host'), '-I' + str(ROOT / 'src/services'),
                str(cfile), *map(str, extra_sources), *extra_flags, '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], cwd=tmp, check=True)

    def test_production_upgrade_transactions(self):
        source = (ROOT / 'src/services/solar_os_module_packages.c').read_text()
        fixture = (ROOT / 'tests/host/pkg_upgrade_test.inc').read_text()
        basics = production_functions(source, (
            'module_set_detail', 'module_name_valid', 'module_type_directory',
            'module_legacy_app_path', 'solar_os_module_package_path',
            'module_foreach_directory', 'solar_os_module_package_foreach_installed',
            'module_download_cancelled', 'module_download_progress',
        ))
        install = production_functions(source, (
            'module_install_one_ex', 'module_install_one', 'module_installed_matches',
            'solar_os_module_package_install', 'solar_os_module_package_remove',
        ))
        storage = production_functions((ROOT / 'src/services/solar_os_storage.c').read_text(), (
            'solar_os_storage_sibling_path', 'solar_os_storage_replace_files',
        ))
        code = fixture.replace('/* BASICS */', basics).replace('/* STORAGE */', storage)
        code = code.replace('/* RECEIPTS */', (ROOT / 'src/services/solar_os_module_receipts.inc').read_text())
        code = code.replace('/* INSTALL */', install)
        code = code.replace('/* UPGRADE */', (ROOT / 'src/services/solar_os_module_upgrade.inc').read_text())
        self.compile_run(code, (CJSON / 'cJSON.c',), ('-lcrypto', '-lm'))

    def test_shell_update_listing_confirmation_and_cancellation(self):
        source = (ROOT / 'src/shell/solar_os_shell_system.c').read_text()
        definitions = source[source.index('typedef struct {', source.index('#define PKG_NETWORK_WAIT_MS')):
                             source.index('static esp_err_t pkg_read_input(')]
        functions = production_functions(source, (
            'pkg_read_input', 'pkg_install_cancelled', 'pkg_install_progress',
            'pkg_print_upgrade_plan', 'pkg_confirm_upgrade', 'pkg_update_or_upgrade',
            'solar_os_shell_cmd_pkg',
        ))
        fixture = (ROOT / 'tests/host/pkg_upgrade_shell_test.inc').read_text()
        self.compile_run(fixture.replace('/* DEFINITIONS */', definitions).replace('/* SHELL */', functions))


if __name__ == '__main__':
    unittest.main()
