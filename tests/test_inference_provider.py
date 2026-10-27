from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


class InferenceProviderTest(unittest.TestCase):
    def test_builtin_and_library_failure_ownership(self):
        with tempfile.TemporaryDirectory() as tmp:
            model=Path(tmp)/'model.espdl'
            model.write_bytes(bytes(16))
            for builtin in (0,1):
                binary=Path(tmp)/f'provider-{builtin}'
                subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    f'-DSOLAR_OS_PACKAGE_BACKEND_INFERENCE_ESPDL={builtin}',
                    '-I'+str(ROOT/'tests/host'),'-I'+str(ROOT/'include'),
                    '-I'+str(ROOT/'src'),
                    '-I'+str(ROOT/'src/services'),
                    str(ROOT/'tests/host/inference_provider_test.c'),
                    str(ROOT/'src/services/solar_os_inference_backend.c'),
                    '-o',str(binary)],check=True)
                subprocess.run([str(binary),str(model)],check=True)
