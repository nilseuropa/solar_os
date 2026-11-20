from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
CJSON=Path.home()/'.platformio/packages/framework-espidf/components/json/cJSON'


class ModuleRequirementsTest(unittest.TestCase):
    def test_production_parser_and_dependency_walk(self):
        with tempfile.TemporaryDirectory() as temp:
            binary=Path(temp)/'requirements'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O1','-g',
                '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                '-I'+str(CJSON),'-I'+str(ROOT/'tests/host'),'-I'+str(ROOT/'src/services'),
                str(ROOT/'tests/host/module_requirements_test.c'),str(CJSON/'cJSON.c'),'-lm','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)


if __name__=='__main__':unittest.main()
