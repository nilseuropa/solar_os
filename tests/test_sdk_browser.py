from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from test_native_sdk import pack,fixture

ROOT=Path(__file__).resolve().parents[1]
HERE=ROOT/'modules/experimental/tinycc'


class SdkBrowserTest(unittest.TestCase):
    def test_browser_and_export_failure_recovery(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            binary=root/'browser'
            subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O1','-g','-Wall','-Wextra','-Werror',
                '-Wno-format-truncation','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                '-I'+str(ROOT/'include'),'-I'+str(HERE),'-I'+str(HERE/'include'),
                str(ROOT/'tests/host/sdk_browser_test.c'),str(HERE/'sdk_cli.c'),str(HERE/'sdk_reader.c'),
                '-o',str(binary)],check=True)
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


if __name__=='__main__':unittest.main()
