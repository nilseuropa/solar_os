#!/usr/bin/env python3
"""Compile the corpus on SolarOS and compare it with an installed GCC provider."""
import argparse
from functools import partial
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import threading

from build import HERE, ROOT, CACHE, run
from file_hil import INSPECTOR
sys.path.insert(0,str(HERE/'differential'))
from corpus import cases, vectors


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(output, seed, random_cases, vector_count):
    suite=cases(seed,random_cases)
    inputs=vectors(seed,vector_count)
    for name in ('abi.h','hil_api.h'):
        shutil.copyfile(HERE/'differential'/name,output/('diff_abi.h' if name=='abi.h' else name))
    (output/'inspect.py').write_text(INSPECTOR)
    cc=Path.home()/'.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc'
    flags=['-std=c11','-fPIC','-fvisibility=hidden','-mlongcalls','-ffp-contract=off',
           '-I',ROOT/'include','-I',output]
    objects=[]
    declarations=[]
    references=[]
    wrapper=(HERE/'differential/hil_main.c').read_text()
    for index,(name,text) in enumerate(suite.items()):
        source=output/(name+'.c'); source.write_text(text)
        pair=[]
        for level in (0,2):
            symbol=f'gcc{level}_{name}'
            obj=output/(symbol+'.o')
            run(cc,*flags,f'-O{level}','-Ddiff_probe='+symbol,'-c',source,'-o',obj)
            objects.append(obj)
            declarations.append(f'uint32_t {symbol}(uint32_t,uint32_t,uint32_t *,const diff_api_t *);\n')
            pair.append(symbol)
        references.append('{'+','.join(pair)+'},\n')
        source.write_text(text+f'\n#define CC_DIFF_CASE {index}\n#define CC_DIFF_NAME "{name}"\n'+wrapper)
    # Use the same target-GCC callback implementations as the emulator harness.
    runner=(HERE/'differential/runner.c').read_text()
    start='static uint32_t mix8('; end='struct observed {'
    if runner.count(start)!=1 or runner.count(end)!=1:
        raise RuntimeError('review differential callback extraction')
    callbacks=start+runner.split(start)[1].split(end)[0]
    provider='#include <stdarg.h>\n#include "solar_os_native_abi.h"\n#include "solar_os_native_library_abi.h"\n#include "hil_api.h"\n'
    provider+=callbacks+''.join(declarations)
    provider+='static const diff_probe_t references[][2]={\n'+''.join(references)+'};\n'
    provider+='static const uint32_t inputs[][2]={\n'+''.join(f'{{{x}U,{y}U}},\n' for x,y in inputs)+'};\n'
    provider+=f'''static const solar_os_cc_hil_api_t hil_api={{1,sizeof(hil_api),&api,{len(suite)},{len(inputs)},inputs,references}};
static int shutdown(void){{return 0;}}
static const solar_os_native_library_descriptor_v1_t descriptor={{
  1,sizeof(descriptor),"tinycc-diff-check","GCC comparison references",
  (const solar_os_native_library_api_header_v1_t *)&hil_api,shutdown}};
int main(void){{return solar_os_native_library_host_v1()->register_library(&descriptor);}}
'''
    (output/'provider.c').write_text(provider)
    run(cc,*flags,'-O2','-c',output/'provider.c','-o',output/'provider.o')
    run(cc,'-nostdlib','-nostartfiles','-shared','-Wl,-Bsymbolic','-Wl,--exclude-libs,ALL',
        '-Wl,-e,main','-Wl,-T,'+str(ROOT/'modules/espdl/layout.ld'),*objects,
        output/'provider.o',CACHE/'support/libgcc.a','-o',output/'provider.elf')
    run(CACHE/'elf-check',output/'provider.elf')
    sys.path.insert(0,str(ROOT/'modules/espdl'))
    from verify import verify
    verify(output/'provider.elf',{'solar_os_native_host_v1','solar_os_native_library_host_v1',
                                 'memcpy','memset','memmove','memcmp','strlen'})
    return suite,inputs


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--telnet',required=True)
    parser.add_argument('--seed',type=lambda s:int(s,0),default=0x534f4c41)
    parser.add_argument('--random-cases',type=int,default=16)
    parser.add_argument('--vectors',type=int,default=128)
    args=parser.parse_args()
    if not 0<=args.random_cases<=128 or not 57<=args.vectors<=4096:
        parser.error('expected 0..128 generated cases and 57..4096 vectors')
    output=CACHE/'differential-hil'; output.mkdir(exist_ok=True)
    report_path=output/'report.json'
    if report_path.exists(): report_path.unlink()
    suite,inputs=prepare(output,args.seed,args.random_cases,args.vectors)
    sys.path.insert(0,str(ROOT/'tests/hil'))
    from model_bundle_stream import Console
    console=Console(args.telnet)
    folder='/dev/cc-differential'; library='/modules/libraries/tinycc-diff-check.elf'
    owned_folder=owned_library=False
    server=None
    log=(output/'console.log').open('w')
    report=None

    def command(text,expected=None):
        log.write('\n> '+text+'\n');log.flush()
        out=console.command(text,60);log.write(out);log.flush()
        if expected is not None and expected not in out: raise RuntimeError(out)
        if not re.search(r'\w+@[^\r\n]+:/[^\r\n]* ',out): raise RuntimeError('missing shell prompt: '+out)
        return out

    def remote_hash(path):
        out=command(f'python {folder}/inspect.py {path}','CC_HASH ')
        found=re.search(r'CC_HASH (\d+) ([0-9a-f]{64})',out)
        if not found: raise RuntimeError('missing artifact hash: '+out)
        return {'bytes':int(found[1]),'sha256':found[2]}

    try:
        command('ls '+folder,'No such file')
        command('ls '+library,'No such file')
        board=command('board');version=command('version');ota=command('ota status')
        if 'ESP32-S3' not in board: raise RuntimeError('requires ESP32-S3')
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as route:
            route.connect((socket.gethostbyname(args.telnet),23));address=route.getsockname()[0]
        server=ThreadingHTTPServer((address,0),partial(SimpleHTTPRequestHandler,directory=str(output)))
        threading.Thread(target=server.serve_forever,daemon=True).start()
        command('mkdir '+folder);owned_folder=True

        def fetch(name,destination):
            out=command(f'curl -o {destination} http://{address}:{server.server_port}/{name}','curl: wrote')
            count=re.search(r'curl: wrote (\d+) bytes',out)
            if 'curl: HTTP 200,' not in out or not count or int(count[1])!=(output/name).stat().st_size:
                raise RuntimeError('failed or incomplete transfer: '+out)

        fetch('inspect.py',folder+'/inspect.py')
        fetch('provider.elf',library);owned_library=True
        if remote_hash(library)['sha256']!=digest(output/'provider.elf'):
            raise RuntimeError('provider transfer hash mismatch')
        for name in ('diff_abi.h','hil_api.h'):
            fetch(name,folder+'/'+name)
            if remote_hash(folder+'/'+name)['sha256']!=digest(output/name):
                raise RuntimeError('header transfer hash mismatch: '+name)
        for name,local in (('tinycc',CACHE/'native/tinycc.library.elf'),('cc',CACHE/'native/cc.app.elf')):
            remote='/modules/'+('libraries/' if name=='tinycc' else 'apps/')+name+'.elf'
            if remote_hash(remote)['sha256']!=digest(local): raise RuntimeError('installed compiler hash mismatch')
        # Warm the compiler and provider before checking repeated request memory.
        results=[]
        before=None
        for index,name in enumerate(suite):
            fetch(name+'.c',folder+'/case.c')
            if remote_hash(folder+'/case.c')['sha256']!=digest(output/(name+'.c')):
                raise RuntimeError('source transfer hash mismatch: '+name)
            command(f'cc {folder}/case.c -I{folder} -o {folder}/case.elf','cc: '+folder+'/case.elf (')
            command('load '+folder+'/case.elf','ABI_HIL_PASS name='+name)
            artifact=remote_hash(folder+'/case.elf')
            command('pkg unload tinycc-diff-check','pkg: unloaded library tinycc-diff-check')
            results.append({'name':name,'vectors':len(inputs),'source_sha256':digest(output/(name+'.c')),**artifact})
            if index==0: before=command('mem')
            print(f'ABI_HIL_CASE {index+1}/{len(suite)} {name} PASS',flush=True)
        after=command('mem')
        free=lambda text:re.search(r'Managed PSRAM: total \d+ free (\d+)',text)
        if not free(before) or not free(after) or free(before)[1]!=free(after)[1]:
            raise RuntimeError('managed memory changed across corpus: '+before+after)
        report={'host':args.telnet,'board':board,'firmware':version,'ota':ota,
                'seed':args.seed,'vectors':len(inputs),'cases':results,'comparisons':len(suite)*len(inputs),
                'provider_sha256':digest(output/'provider.elf'),'gcc_flags':['-O0','-O2','-ffp-contract=off'],
                'compiler':json.loads((CACHE/'native/build.json').read_text()),
                'sources':{str(p.relative_to(HERE)):digest(p) for p in
                           [Path(__file__),HERE/'differential/corpus.py',HERE/'differential/runner.c',
                            HERE/'differential/hil_api.h',HERE/'differential/hil_main.c',HERE/'differential/abi.h']},
                'before':before,'after':after,'file_compilation_on_device':True,'gcc_references_on_device':True}
    finally:
        try:
            if owned_library:
                command('pkg unload tinycc-diff-check')
                command('rm '+library)
            if owned_folder:
                for name in ('case.c','case.elf','inspect.py','diff_abi.h','hil_api.h'): command('rm '+folder+'/'+name)
                command('rm -f '+folder)
        finally:
            if server:server.shutdown();server.server_close()
            console.socket.close();log.close()
    report['temporary_files_removed']=True
    report_path.write_text(json.dumps(report,indent=2)+'\n')
    print(f'SOLAR_CC_DIFFERENTIAL_HIL_PASS: {len(suite)} cases x {len(inputs)} vectors',flush=True)


if __name__=='__main__':main()
