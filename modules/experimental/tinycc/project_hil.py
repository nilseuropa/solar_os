#!/usr/bin/env python3
"""Install/test cc project builds on sol (SolarTerm) with the current SDK.

Backs up the installed cc ELF and leaves the new client plus useful project
examples installed. Refuses existing example/app paths. Does not flash firmware,
change repositories or publish packages. Requires idle displayd and ftpd jobs.
"""
import argparse
import ftplib
import hashlib
import io
import json
from pathlib import Path
import re
import select
import sys
import time
import urllib.request

from build import ROOT, HERE, CACHE


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--output', type=Path, default=CACHE/'project-hil')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(ROOT/'tests/hil'))
    from model_bundle_stream import Console
    console = Console(args.host)
    ftp = None
    owned_ftp = owned_display = False
    sessions = []
    code = None
    logs = []
    folder = '/dev/cc-project'
    demo = HERE/'examples/build-project'
    manifest = (demo/'cc.toml').read_bytes()
    source = (demo/'src/main.c').read_bytes()
    project_created = False

    def command(text, expected=None):
        out = console.command(text, 60)
        logs.append('> '+text+'\n'+out)
        (args.output/'console.log').write_text('\n'.join(logs))
        if expected is not None:
            assert expected in out, (text,out)
        assert re.search(r'\w+@[^\r\n]+:/[^\r\n]* ',out), (text,out)
        return out

    def get(path):
        data = io.BytesIO()
        ftp.retrbinary('RETR '+path,data.write)
        return data.getvalue()

    def put(path,data):
        ftp.storbinary('STOR '+path,io.BytesIO(data))

    def exists(path):
        try:
            ftp.size(path)
            return True
        except ftplib.error_perm:
            try:
                old = ftp.pwd()
                ftp.cwd(path)
                ftp.cwd(old)
                return True
            except ftplib.error_perm:
                return False

    def keys(data):
        request = urllib.request.Request(f'http://{args.host}/api/displays/display0/input',
            data=data,headers={'Authorization':'Bearer '+code})
        with urllib.request.urlopen(request,timeout=10) as response:
            assert response.status==204
        time.sleep(.4)

    def frame(name):
        request = urllib.request.Request(f'http://{args.host}/api/displays/display0/frame.pbm',
            headers={'Authorization':'Bearer '+code})
        with urllib.request.urlopen(request,timeout=10) as response:
            data = response.read()
        from PIL import Image
        picture = Image.open(io.BytesIO(data))
        picture.save(args.output/name)
        return picture.crop((40,40,picture.width-40,picture.height-40)).tobytes()

    def managed():
        text = command('mem')
        found = re.search(r'Managed PSRAM: total (\d+) free (\d+) largest (\d+) resident (\d+)',text)
        assert found,text
        return tuple(map(int,found.groups()))

    def cancel_build(text):
        console.socket.sendall((text+'\r\n').encode())
        output = bytearray()
        sent = False
        deadline = time.monotonic()+60
        while time.monotonic()<deadline:
            if not select.select([console.socket],[],[],.1)[0]:
                continue
            data = console.socket.recv(65536)
            assert data,'console ended'
            output.extend(data)
            if not sent and b'cc: compiling ' in output:
                console.socket.sendall(b'\x03')
                sent = True
            if re.search(rb'\w+@[^\r\n]+:/[^\r\n]* ',output):
                break
        result = output.decode(errors='replace')
        logs.append('> '+text+'\n'+result)
        (args.output/'console.log').write_text('\n'.join(logs))
        assert sent and 'compilation failed (-7)' in result,result

    try:
        board = command('board','SolarTerm')
        assert 'solar_term' in board and '@sol:' in board,board
        ota = command('ota status')
        jobs = command('job status')
        for name in ('ftpd','displayd'):
            assert not re.search(r'^'+name+r'\s+running',jobs,re.M), name+' is already owned'
        command('job start ftpd /sdcard 2121','job started: ftpd')
        owned_ftp = True
        ftp = ftplib.FTP()
        ftp.connect(args.host,2121,timeout=15)
        ftp.login()
        assert not exists(folder),folder+' already exists'
        assert not exists('/modules/apps/project-cube.elf'),'project-cube already exists'
        previous = get('/modules/apps/cc.elf')
        backup = args.output/'cc-before.elf'
        if backup.exists():
            assert backup.read_bytes()==previous,'a different cc backup already exists'
        else:
            backup.write_bytes(previous)
        compiler_hash = hashlib.sha256(get('/modules/libraries/tinycc.elf')).hexdigest()
        build = json.loads((CACHE/'native/build.json').read_text())
        assert compiler_hash==build['artifacts']['tinycc.library.elf']['sha256'], 'install matching TinyCC first'
        client = (CACHE/'native/cc.app.elf').read_bytes()
        put('/modules/apps/cc.elf',client)
        assert get('/modules/apps/cc.elf')==client
        command('cc --help','cc build [path/to/cc.toml]')
        listing = command('cc sdk list','hello-library 1.1.0')
        assert 'jet 1.12.0' in listing,listing
        ftp.mkd(folder)
        project_created = True
        for path in ('src','include'):
            ftp.mkd(folder+'/'+path)
        for path in sorted(demo.rglob('*')):
            if path.is_file():
                put(folder+'/'+path.relative_to(demo).as_posix(),path.read_bytes())
        command('cd '+folder)
        command('cc build','cc: /sdcard'+folder+'/build/demo.elf (')
        elf = get(folder+'/build/demo.elf')
        (args.output/'demo.elf').write_bytes(elf)
        command('cc '+folder+'/src/main.c '+folder+'/src/helpers.c -I '+folder+'/include -o '+folder+'/build/direct.elf','/build/direct.elf (')
        assert get(folder+'/build/direct.elf')==elf
        command('load build/demo.elf','PROJECT_BUILD_PASS: library=42')
        command('pkg unload hello-library','unloaded')
        command('cd /')
        command('cc build '+folder+'/cc.toml','/build/demo.elf (')
        assert get(folder+'/build/demo.elf')==elf
        installed = command('pkg installed')
        assert 'library hello-library (on storage)' in installed,installed
        # Paths containing spaces are values, rather than shell arguments.
        put(folder+'/src/main copy.c',source)
        put(folder+'/cc.toml',manifest.replace(b'src/main.c',b'src/main copy.c'))
        command('cc build '+folder+'/cc.toml','/build/demo.elf (')
        ftp.delete(folder+'/src/main copy.c')
        put(folder+'/cc.toml',manifest)
        command('cc build '+folder+'/cc.toml','/build/demo.elf (')
        assert get(folder+'/build/demo.elf')==elf
        checks = ['two translation units','direct cc invocation compatibility','project include directory','output directory creation',
            'native library execution','manifest-relative paths from another cwd',
            'source filename with spaces','SDK checks do not load providers']
        for changed, expected in (
            (manifest+b'unknown = true\n','unknown project key'),
            (manifest+b'output = "other.elf"\n','duplicate project key'),
            (manifest.replace(b'1.1.0',b'999.0'),'installed 1.1.0'),
            (manifest.replace(b'hello-library',b'cc-project-missing'),'pkg install cc-project-missing'),
            (manifest.replace(b'build/demo.elf',b'CC.TOML'),'project file'),
            (manifest.replace(b'build/demo.elf',b'SRC/MAIN.C'),'every source file'),
        ):
            put(folder+'/cc.toml',changed)
            command('cc build '+folder+'/cc.toml',expected)
            assert get(folder+'/build/demo.elf')==elf
            assert get(folder+'/src/main.c')==source
            assert get(folder+'/cc.toml')==changed
        put(folder+'/cc.toml',manifest)
        put(folder+'/src/main.c',b'int broken(void) { return missing + ; }\n')
        command('cc build '+folder+'/cc.toml','compilation failed')
        assert get(folder+'/build/demo.elf')==elf
        put(folder+'/cc.toml',manifest.replace(b'build/demo.elf',b'failed-build/demo.elf'))
        command('cc build '+folder+'/cc.toml','compilation failed')
        assert not exists(folder+'/failed-build')
        put(folder+'/cc.toml',manifest)
        # Cancel after compilation starts, independently of SD metadata latency.
        large = source+b'\n'+b'\n'.join(f'int extra_{i}(int x) {{ return x+{i}; }}'.encode() for i in range(250))
        put(folder+'/src/main.c',large)
        cancel_build('cc build '+folder+'/cc.toml')
        assert get(folder+'/build/demo.elf')==elf
        put(folder+'/src/main.c',source)
        command('cc build '+folder+'/cc.toml','/build/demo.elf (')
        assert get(folder+'/build/demo.elf')==elf
        checks += ['unknown and duplicate key diagnostics','exact SDK version mismatch','missing dependency',
            'project/source output collision protection on FAT','failed build preserves ELF',
            'failed compilation does not create directories','cancellation preserves ELF','recovery after cancellation']
        command('pkg unload tinycc','unloaded')
        baseline = managed()
        for _ in range(3):
            command('cc build '+folder+'/cc.toml','/build/demo.elf (')
            command('pkg unload tinycc','unloaded')
            assert managed()==baseline
        checks.append('three build/unload cycles preserve managed PSRAM')
        # Exercise the same project command for a resumable GUI using Jet.
        ftp.mkd(folder+'/graphics')
        graphic = (HERE/'examples/jet-cube/main.c').read_bytes().replace(b'jet-cube',b'project-cube')
        put(folder+'/graphics/main.c',graphic)
        put(folder+'/graphics/cc.toml',(HERE/'examples/jet-cube/cc.toml').read_bytes().replace(b'jet-cube',b'project-cube'))
        command('cc build '+folder+'/graphics/cc.toml','/modules/apps/project-cube.elf (')
        (args.output/'project-cube.elf').write_bytes(get('/modules/apps/project-cube.elf'))
        reply = command('job start displayd display0','access code:')
        owned_display = True
        code = re.search(r'access code: (\d+)',reply)[1]
        reply = command('session create project-cube display0 --paused','created')
        sid = re.search(r'session (\d+) created',reply)[1]
        sessions.append(sid)
        time.sleep(1)
        before = frame('project-cube.png')
        keys(bytes([0x83]))
        assert frame('project-cube-rotated.png')!=before
        keys(b'q')
        assert not re.search(r'^'+sid+r'\s',command('session list'),re.M)
        sessions.remove(sid)
        command('pkg unload jet','unloaded')
        command('pkg unload tinycc','unloaded')
        checks += ['Jet SDK dependency','project-built GUI launch','visible rendering and input','clean GUI exit']
        report = dict(host=args.host,board=board,ota=ota,checks=checks,
            client_version=build['client_version'],compiler_version=build['version'],
            client_sha256=hashlib.sha256(client).hexdigest(),compiler_sha256=compiler_hash,
            old_client_sha256=hashlib.sha256(previous).hexdigest(),
            output_sha256=hashlib.sha256(elf).hexdigest(),memory_baseline=baseline,top=command('top'))
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        print('On-device project builds: PASS',flush=True)
    finally:
        # Only restore files created by this run, leaving the usable examples.
        if ftp and project_created:
            put(folder+'/cc.toml',manifest)
            put(folder+'/src/main.c',source)
        for sid in sessions:
            try: command('session close '+sid)
            except Exception: pass
        if owned_display:
            try: command('job stop displayd')
            except Exception: pass
        if ftp:
            ftp.close()
        if owned_ftp:
            try: command('job stop ftpd')
            except Exception: pass
        console.socket.close()


if __name__=='__main__':
    main()
