from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

ROOT=Path(__file__).resolve().parents[1]
CC=Path.home()/'.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc'


class CompilerPackagesTest(unittest.TestCase):
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
