"""Services can be selected independently of their shell frontends."""
from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ShellFrontendOwnershipTest(unittest.TestCase):
    def test_disabled_service_frontends_compile_without_backend_headers(self):
        packages = tomllib.loads((ROOT / 'packages/solar_os_packages.toml').read_text())['packages']
        core_shell = packages['core_shell']['sources']
        # A frontend owned by the shell must disappear completely when its
        # service is absent, rather than retaining undeclared SDK dependencies.
        frontends = []
        for name in core_shell:
            path = ROOT / 'src' / name
            if path.suffix == '.c' and path.read_text().startswith('#include "solar_os_config.h"\n\n#if SOLAR_OS_PACKAGE_SERVICE_'):
                frontends.append(path)
        self.assertTrue(frontends)
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / 'solar_os_config.h').write_text(''.join(
                '#define SOLAR_OS_PACKAGE_' + name.upper() + ' 0\n' for name in packages))
            for index, path in enumerate(frontends):
                with self.subTest(frontend=path.name):
                    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(stage),
                                    '-c', str(path), '-o', str(stage / (str(index) + '.o'))], check=True)

    def test_service_units_have_no_shell_source_dependency(self):
        packages = tomllib.loads((ROOT / 'packages/solar_os_packages.toml').read_text())['packages']
        for name, spec in packages.items():
            if name.startswith('service_'):
                self.assertFalse([source for source in spec.get('sources', []) if source.startswith('shell/')], name)
