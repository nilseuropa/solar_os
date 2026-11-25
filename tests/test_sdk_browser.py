from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from test_native_sdk import pack,fixture

ROOT=Path(__file__).resolve().parents[1]
HERE=ROOT/'modules/experimental/tinycc'


def build_browser(binary, builtin_index=None):
    subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O1','-g','-Wall','-Wextra','-Werror',
        '-Wno-format-truncation','-fsanitize=address,undefined','-fno-omit-frame-pointer',
        *(['-DSOLAR_OS_TEST_BUILTIN_SDK', '-I'+str(builtin_index.parent)] if builtin_index else []),
        '-I'+str(ROOT/'include'),'-I'+str(HERE),'-I'+str(HERE/'include'),
        str(ROOT/'tests/host/sdk_browser_test.c'),str(HERE/'sdk_cli.c'),str(HERE/'sdk_reader.c'),
        '-o',str(binary)],check=True)


class SdkBrowserTest(unittest.TestCase):
    def test_browser_and_export_failure_recovery(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            binary=root/'browser'
            build_browser(binary)
            module=root/'module';module.mkdir()
            manifest=module/'module.toml';manifest.write_text('type="library"\nid="testlib"\nversion="1.2.3"\n'
                '[sdk.headers]\n"api.h"="api.h"\n"detail/inner.h"="inner.h"\n')
            (module/'api.h').write_text('#define ANSWER 42\n');(module/'inner.h').write_text('#include "../api.h"\n')
            data=pack.pack_elf(fixture(),manifest)
            for mode in ('valid','nul','broken','traversal','truncated','collision'):
                with self.subTest(mode=mode):
                    folder=root/mode;libraries=folder/'modules/libraries';libraries.mkdir(parents=True)
                    artifact=bytearray(data)
                    if mode=='nul':artifact[256+96+2*104]=0
                    if mode=='broken':artifact[256]=ord('X')
                    if mode=='traversal':artifact[256+96:256+96+len(b'testlib/api.h')]=b'../filexxxxx\0'
                    if mode=='truncated':artifact=artifact[:-1]
                    if mode=='collision':
                        manifest.write_text(manifest.read_text()+'"API.h"="api.h"\n')
                        artifact=pack.pack_elf(fixture(),manifest)
                    (libraries/'testlib.elf').write_bytes(artifact)
                    (libraries/'plain.elf').write_bytes(fixture())
                    subprocess.run([str(binary),str(folder),mode],check=True)

    def test_robot_headers_are_discoverable_and_self_contained(self):
        manifest=ROOT/'modules/robot/module.toml'
        module_id,version,headers=pack.load_headers(manifest,ROOT/'include')
        self.assertEqual((module_id,version),('robot','1.0.0'))
        self.assertEqual({name for name,_,_ in headers},{'robot/'+name for name in (
            'solar_os_robot_abi.h','solar_os_robot_model.h','solar_os_robot_diff_drive.h')})
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);libraries=root/'modules/libraries';libraries.mkdir(parents=True)
            (libraries/'robot.elf').write_bytes(pack.pack_elf(fixture(),manifest,ROOT/'include'))
            binary=root/'browser';build_browser(binary)
            subprocess.run([str(binary),str(root),'robot'],check=True)
            for name,data,_ in headers:
                self.assertEqual((root/'sdk'/name).read_bytes(),data)
            # Match TinyCC's built-in native headers, without adding the
            # repository include directory and masking missing library headers.
            for path in (ROOT/'include').glob('solar_os_native*abi.h'):
                (root/'sdk'/path.name).write_bytes(path.read_bytes())
            source=root/'consumer.c'
            source.write_text('''#include <robot/solar_os_robot_abi.h>
int inspect(const solar_os_robot_api_v3_t *api, uint32_t model, uint32_t drive) {
    solar_os_robot_model_info_t info;
    solar_os_robot_diff_drive_status_t status;
    return api->model_info(model, &info) || api->diff_drive_status(drive, &status);
}
''')
            compiler=Path.home()/'.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc'
            subprocess.run([str(compiler) if compiler.is_file() else 'cc',
                '-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'sdk'),
                '-c',str(source),'-o',str(root/'consumer.o')],check=True)


if __name__=='__main__':unittest.main()
