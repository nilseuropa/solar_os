from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest
from test_flavor_packages import generate_flavor_config

ROOT=Path(__file__).resolve().parents[1]
CC=Path.home()/'.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc'


class CompilerPackagesTest(unittest.TestCase):
    def test_builtin_compiler_defaults_to_full_and_prunes_unsupported_boards(self):
        catalog = generate_flavor_config.load_catalog(ROOT/'packages/solar_os_packages.toml')
        for path in (ROOT/'flavors').glob('*.toml'):
            _, _, groups, packages = generate_flavor_config.load_flavor(path, catalog)
            expected = path.stem == 'full'
            self.assertEqual(groups['cc'], expected, path.name)
            self.assertEqual(packages['app_cc'], expected, path.name)
        with tempfile.TemporaryDirectory() as temporary:
            flavor = Path(temporary)/'cc.toml'
            flavor.write_text('[flavor]\nname="cc-only"\n[groups]\ncc=true\n')
            _, _, groups, packages = generate_flavor_config.load_flavor(flavor, catalog)
            self.assertTrue(packages['app_cc'] and packages['service_native_modules'])
            for target, capabilities, expected in (('esp32s3', {'psram', 'wifi'}, True),
                                                    ('esp32s3', {'wifi'}, False),
                                                    ('esp32', {'psram', 'wifi'}, False)):
                selected = generate_flavor_config.apply_target_pruning(catalog, packages, target)
                _, selected = generate_flavor_config.apply_board_capability_pruning(catalog, groups, selected, capabilities)
                self.assertEqual(selected['app_cc'], expected)

    @unittest.skipUnless(CC.is_file(),'ESP32-S3 toolchain required')
    def test_requirements_match_target_service_tables(self):
        headers=('service','thread','time','libc','storage','library')
        code=['#include "solar_os_native_'+h+'_abi.h"' for h in headers]
        for module in ('cc','tinycc','jet'):
            meta=tomllib.loads((ROOT/'modules'/module/'module.toml').read_text())
            for service in meta['required_services']:
                name=service['name']
                typename='solar_os_native_'+('library_service' if name=='library' else name)+'_api_v1_t'
                size=f'offsetof({typename},parse_float)' if module=='cc' and name=='libc' else f'sizeof({typename})'
                code.append(f'_Static_assert({size}=={service["minimum_struct_size"]},"{module}: {name} requirement drift");')
        with tempfile.TemporaryDirectory() as temp:
            source=Path(temp)/'services.c';source.write_text('\n'.join(code)+'\n')
            subprocess.run([str(CC),'-I'+str(ROOT/'include'),'-c',str(source),'-o',str(Path(temp)/'services.o')],check=True)


if __name__=='__main__':unittest.main()
