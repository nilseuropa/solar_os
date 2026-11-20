#!/usr/bin/env python3
"""Validate installed library headers, multi-file builds and the C SDK on SolarOS."""
import argparse
from contextlib import nullcontext
from functools import partial
import hashlib
from http.server import SimpleHTTPRequestHandler,ThreadingHTTPServer
import json
from pathlib import Path
import re
import shutil
import socket
import sys
import tempfile
import threading

from build import HERE,ROOT,CACHE
from file_hil import INSPECTOR


def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--telnet',required=True)
    args=parser.parse_args()
    output=CACHE/'sdk-hil';output.mkdir(exist_ok=True)
    report_path=output/'report.json'
    if report_path.exists():report_path.unlink()
    sys.path.insert(0,str(ROOT/'tests/hil'))
    from model_bundle_stream import Console
    c=Console(args.telnet);server=None;temporary=None
    log=(output/'console.log').open('w')
    folder='/dev/cc-sdk-project';library='/modules/libraries/hello-library.elf'

    def command(text,expected=None):
        log.write('\n> '+text+'\n');log.flush()
        out=c.command(text,60);log.write(out);log.flush()
        if expected and expected not in out:raise RuntimeError(out)
        if not re.search(r'\w+@[^\r\n]+:/[^\r\n]* ',out):raise RuntimeError('missing prompt: '+out)
        return out

    def remote_hash(path):
        if not path.startswith('/'):path=folder+'/'+path
        out=command(f'python {folder}/inspect.py {path}','CC_HASH ')
        found=re.search(r'CC_HASH (\d+) ([0-9a-f]{64})',out)
        if not found:raise RuntimeError(out)
        return found[2]

    report=None;library_valid=True
    try:
        command('ls '+folder,'No such file')
        board=command('board');version=command('version');ota=command('ota status')
        if 'ESP32-S3' not in board:raise RuntimeError('requires ESP32-S3')
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as route:
            route.connect((socket.gethostbyname(args.telnet),23));address=route.getsockname()[0]
        temporary=tempfile.TemporaryDirectory(prefix='solaros-c-sdk-')
        with nullcontext(temporary.name) as temp:
            stage=Path(temp)
            for path in (HERE/'examples/project').iterdir():shutil.copyfile(path,stage/path.name)
            for name in ('sdk_check.c','sdk_aux.c','sdk_io_check.c'):shutil.copyfile(HERE/'examples'/name,stage/name)
            (stage/'bad.c').write_text('int broken(void){return absent;}\n')
            (stage/'duplicate.c').write_text('int main(void){return 0;}\n')
            (stage/'inspect.py').write_text(INSPECTOR)
            shutil.copyfile(CACHE/'hello-library.library.elf',stage/'library.elf')
            # Corrupt metadata without changing executable code or ELF bounds.
            import struct
            data=bytearray((stage/'library.elf').read_bytes())
            table=struct.unpack_from('<I',data,32)[0];count,names=struct.unpack_from('<HH',data,48)
            sections=[struct.unpack_from('<10I',data,table+i*40) for i in range(count)]
            strings=sections[names]
            for section in sections:
                at=strings[4]+section[0];name=data[at:data.index(0,at)]
                if name==b'.solaros.sdk':data[section[4]]=ord('X');break
            else:raise RuntimeError('provider has no packaged SDK')
            (stage/'bad-library.elf').write_bytes(data)
            server=ThreadingHTTPServer((address,0),partial(SimpleHTTPRequestHandler,directory=temp))
            threading.Thread(target=server.serve_forever,daemon=True).start()
            command('mkdir '+folder)

            def fetch(name,path):
                out=command(f'curl -o {path} http://{address}:{server.server_port}/{name}','curl: HTTP 200,')
                match=re.search(r'curl: wrote (\d+) bytes',out)
                if not match or int(match[1])!=(stage/name).stat().st_size:raise RuntimeError('incomplete transfer: '+out)

            fetch('inspect.py',folder+'/inspect.py')
            for name in ('main.c','helpers.c','project.h','sdk_check.c','sdk_aux.c','sdk_io_check.c','bad.c','duplicate.c'):
                fetch(name,folder+'/'+name)
                if remote_hash(folder+'/'+name)!=digest(stage/name):raise RuntimeError('source transfer mismatch: '+name)
            for remote,local in (('/modules/apps/cc.elf',CACHE/'native/cc.app.elf'),
                                 ('/modules/libraries/tinycc.elf',CACHE/'native/tinycc.library.elf'),
                                 (library,stage/'library.elf')):
                if remote_hash(remote)!=digest(local):raise RuntimeError('installed artifact mismatch: '+remote)
            command('cd '+folder)
            command('cc main.c helpers.c -o project.elf','cc: project.elf (')
            command('load project.elf','SDK_PROJECT_PASS library=42 sqrt=9.00')
            expected=hashlib.sha256(b'library=42 sqrt=9.00\n').hexdigest()
            if remote_hash('result.txt')!=expected:raise RuntimeError('file output mismatch')
            project_hash=remote_hash('project.elf')
            # No library header exists on storage; include resolution reads its ELF.
            command('ls '+folder+'/hello_library_api.h','No such file')
            command('cc sdk_aux.c sdk_check.c -o runtime.elf','cc: runtime.elf (')
            command('load runtime.elf','SDK_C_RUNTIME_PASS')
            command('cc sdk_io_check.c -o io.elf','cc: io.elf (')
            command('load io.elf','SDK_IO_PASS')
            if remote_hash('auto.txt')!=hashlib.sha256(b'automatic flush\n').hexdigest():
                raise RuntimeError('native main did not flush and close files')
            for text,expected in (('cc main.c bad.c -o project.elf','/bad.c:1:'),
                                  ('cc main.c duplicate.c -o project.elf','defined twice'),
                                  ('cc main.c missing.c -o project.elf','not found'),
                                  ('cc main.c helpers.c -o helpers.c','output must differ')):
                command(text,expected)
                if remote_hash('project.elf')!=project_hash:raise RuntimeError('failed build replaced output')
            if remote_hash('helpers.c')!=digest(stage/'helpers.c'):raise RuntimeError('output path overwrote a source file')
            command('pkg unload hello-library','pkg: unloaded library hello-library')
            command('pkg remove hello-library','pkg: removed hello-library');library_valid=False
            command('cc main.c helpers.c -o project.elf','hello-library/hello_library_api.h')
            if remote_hash('project.elf')!=project_hash:raise RuntimeError('missing SDK replaced output')
            fetch('bad-library.elf',library)
            command('cc main.c helpers.c -o project.elf','could not read include file')
            if remote_hash('project.elf')!=project_hash:raise RuntimeError('corrupt SDK replaced output')
            fetch('library.elf',library);library_valid=True
            command('cc main.c helpers.c -o project.elf','cc: project.elf (')
            command('load project.elf','SDK_PROJECT_PASS')
            before=command('mem')
            for _ in range(5):
                command('cc main.c helpers.c -o project.elf','cc: project.elf (')
                command('load project.elf','SDK_PROJECT_PASS')
                command('load io.elf','SDK_IO_PASS')
            after=command('mem')
            free=lambda text:re.search(r'Managed PSRAM: total \d+ free (\d+)',text)
            if not free(before) or not free(after) or free(before)[1]!=free(after)[1]:
                raise RuntimeError('managed memory changed: '+before+after)
            report={'host':args.telnet,'board':board,'firmware':version,'ota':ota,
                'compiler_build':json.loads((CACHE/'native/build.json').read_text()),
                'sources':{str(p.relative_to(HERE)):digest(p) for p in
                    [Path(__file__),*sorted((HERE/'examples/project').iterdir()),HERE/'examples/sdk_check.c',
                     HERE/'examples/sdk_aux.c',HERE/'examples/sdk_io_check.c']},
                'provider_sha256':digest(stage/'library.elf'),'project_sha256':remote_hash('project.elf'),
                'runtime_sha256':remote_hash('runtime.elf'),'io_sha256':remote_hash('io.elf'),
                'installed_headers_discovered':True,'two_translation_units':True,'standard_c_runtime':True,
                'file_io_and_automatic_close':True,'error_preserves_output':True,'corrupt_sdk_rejected':True,
                'uninstall_removes_sdk':True,'repeated_compile_run':5,'before':before,'after':after,
                'examples_left_installed':folder}
            for name in ('inspect.py','bad.c','duplicate.c','data.bin','limits.txt','slots.txt','auto.txt'):
                command('rm '+folder+'/'+name)
            report_path.write_text(json.dumps(report,indent=2)+'\n')
    finally:
        if not library_valid and server:
            try:command(f'curl -o {library} http://{address}:{server.server_port}/library.elf','curl: HTTP 200,')
            except Exception:pass
        if server:server.shutdown();server.server_close()
        if temporary:temporary.cleanup()
        c.socket.close();log.close()
    print('SOLAR_CC_SDK_HIL_PASS: installed headers, two-file app, C runtime, file I/O and recovery',flush=True)


if __name__=='__main__':main()
