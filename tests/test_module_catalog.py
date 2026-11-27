import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions
from host_cjson import CJSON

ROOT = Path(__file__).resolve().parents[1]


class ModuleCatalogTest(unittest.TestCase):
    def test_real_parser_scales_allocations_and_cleans_up_every_failure(self):
        source = (ROOT / 'src/services/solar_os_module_packages.c').read_text()
        basics = production_functions(source, ('module_name_valid', 'solar_os_module_type_name',
            'module_type_parse', 'module_relative_path_valid', 'module_join_url', 'module_set_detail'))
        parser = production_functions(source, ('module_parse_package', 'module_parse_catalog',
            'module_catalog_decode', 'solar_os_module_catalog_free'))
        fixture = (ROOT / 'tests/host/module_catalog_test.c').read_text()
        code = fixture.replace('/* BASICS */', basics).replace('/* PARSER */', parser)
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / 'catalog.c').write_text(code)
            binary = stage / 'catalog'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-Wno-unused-function', '-g', '-O1', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-I', str(stage), '-I', str(ROOT / 'tests/host/native_abi_stubs'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                '-I', str(CJSON), str(stage / 'catalog.c'), str(ROOT / 'src/services/solar_os_json.c'),
                str(CJSON / 'cJSON.c'), '-lm', '-o', str(binary)], check=True)

            def document(count, services=0):
                modules = []
                for index in range(count):
                    package = dict(type='app', id=f'app{index}', name='Application', version='1.0.0',
                        description='Application package', lifecycle_abi=1, minimum_host_api_size=8,
                        artifact=dict(path=f'app/app{index}/1.0.0/{"a" * 64}.elf', size=64, sha256='a' * 64),
                        required_capabilities=[], required_services=[dict(name=f'service{s}', abi=1,
                            minimum_struct_size=8) for s in range(services)], dependencies=[])
                    modules.append(package)
                return dict(schema='solaros.module_catalog', schema_version=3, project='SolarOS',
                    host=dict(version='test', source_commit='a' * 40, target='host', native_abi=1), modules=modules)

            cases = [(document(count), 0, count) for count in (0, 1, 17, 256)]
            cases += [(document(1, 64), 0, 1), (document(257), 0x104, 0), (document(1, 65), 0x104, 0)]
            malformed = document(17)
            malformed['modules'][-1]['artifact']['sha256'] = 'invalid'
            cases.append((malformed, 0x108, 0))
            duplicate = document(17)
            duplicate['modules'][-1] = copy.deepcopy(duplicate['modules'][0])
            cases.append((duplicate, 0x108, 0))
            for number, (body, expected, count) in enumerate(cases):
                with self.subTest(case=number, count=count):
                    path = stage / 'catalog.json'
                    path.write_text(json.dumps(body))
                    subprocess.run([str(binary), str(path), str(expected), str(count)], check=True, timeout=20)
