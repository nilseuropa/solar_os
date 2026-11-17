#!/usr/bin/env python3
"""Validate a locally signed package catalog and an on-device C foreground app.

Requires firmware built with a repository URL pointing at this server, an idle
display, and stopped displayd/ftpd jobs. Back up installed modules and catalog
before running: this test replaces cc, tinycc and Jet and leaves them installed.
Only the source example is transferred outside pkg; its ELF is built on device.
"""
import argparse
import copy
import ftplib
from functools import partial
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import threading
import time
import urllib.request

from build import ROOT, HERE


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host',required=True)
    p.add_argument('--address',required=True)
    p.add_argument('--port',type=int,default=8873)
    p.add_argument('--repository',type=Path,required=True)
    p.add_argument('--signing-key',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--reuse-example',action='store_true',help='Replace this test\'s existing example source')
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    (args.output/'report.json').unlink(missing_ok=True)
    sys.path.insert(0,str(ROOT/'tests/hil'))
    from model_bundle_stream import Console
    catalog_path=next(args.repository.glob('modules/*/catalog.json'))
    base=json.loads(catalog_path.read_text())
    requests=[]
    class Handler(SimpleHTTPRequestHandler):
        def do_GET(self):
            requests.append(self.path);super().do_GET()
        def log_message(self,*args):pass
    server=ThreadingHTTPServer((args.address,args.port),partial(Handler,directory=str(args.repository)))
    threading.Thread(target=server.serve_forever,daemon=True).start()
    console=Console(args.host)
    log=(args.output/'console.log').open('w')
    sessions=[];display_started=False
    folder='/dev/jet-c-sdk'
    def command(text,expected=None,seconds=60):
        log.write('\n> '+text+'\n');log.flush()
        out=console.command(text,seconds);log.write(out);log.flush()
        if expected and expected not in out:raise AssertionError(text+'\n'+out)
        if not re.search(r'\w+@[^\r\n]+:/[^\r\n]* ',out):raise AssertionError('No prompt: '+out)
        return out
    def publish(catalog):
        catalog_path.write_text(json.dumps(catalog,separators=(',',':'))+'\n')
        sig=subprocess.check_output(['openssl','dgst','-sha256','-sign',str(args.signing_key),str(catalog_path)])
        import base64
        catalog_path.with_name('catalog.sig').write_text(base64.b64encode(sig).decode()+'\n')
    def remote(path):
        command('job start ftpd /sdcard 2121','job started: ftpd')
        try:
            data=io.BytesIO()
            with ftplib.FTP() as ftp:
                ftp.connect(args.host,2121,timeout=15);ftp.login()
                ftp.retrbinary('RETR '+path,data.write)
            return data.getvalue()
        finally:command('job stop ftpd','job stopped: ftpd')
    def sha(path):return hashlib.sha256(remote(path)).hexdigest()
    def create(app,arguments=''):
        out=command(f'session create {app} display0 {arguments}')
        found=re.search(r'session (\d+) created',out);assert found,out
        sessions.append(found[1]);time.sleep(1)
        return found[1]
    def close(sid):
        command('session close '+sid,'closed session');sessions.remove(sid)
    def key(data):
        request=urllib.request.Request(f'http://{args.host}/api/displays/display0/input',data=data,
            headers={'Authorization':'Bearer '+code})
        with urllib.request.urlopen(request,timeout=10) as response:assert response.status==204
        time.sleep(.35)
    def frame(name):
        request=urllib.request.Request(f'http://{args.host}/api/displays/display0/frame.pbm',
            headers={'Authorization':'Bearer '+code})
        with urllib.request.urlopen(request,timeout=10) as response:data=response.read()
        assert data.startswith(b'P4\n'),data[:80]
        (args.output/(name+'.pbm')).write_bytes(data)
        from PIL import Image
        picture=Image.open(io.BytesIO(data));picture.save(args.output/(name+'.png'))
        return picture.crop((40,40,picture.width-40,picture.height-40)).tobytes()
    try:
        jobs=command('job status')
        for job in ('displayd','ftpd'):
            assert not re.search(r'^'+job+r'\s+running',jobs,re.M),job+' is already owned'
        if not args.reuse_example:command('ls '+folder,'No such file')
        board=command('board');ota=command('ota status');command('session list')
        assert 'SolarTerm' in board and 'solar_term' in board
        before={name:sha('/modules/'+name) for name in ('apps/cc.elf','libraries/tinycc.elf','libraries/jet.elf')}
        unsupported=copy.deepcopy(base)
        tiny=next(m for m in unsupported['modules'] if m['id']=='tinycc')
        next(s for s in tiny['required_services'] if s['name']=='libc')['minimum_struct_size']=60
        publish(unsupported)
        at=len(requests)
        command('pkg install cc','needs service libc')
        assert not any(path.endswith('.elf') for path in requests[at:]),requests[at:]
        wrong=copy.deepcopy(base)
        next(m for m in wrong['modules'] if m['id']=='cc')['dependencies'][0]['version']='999.0'
        publish(wrong);at=len(requests)
        command('pkg install cc','module catalog is invalid')
        assert not any(path.endswith('.elf') for path in requests[at:])
        assert before=={name:sha('/modules/'+name) for name in before}
        publish(base)
        command('pkg unload tinycc');command('pkg unload jet')
        command('pkg remove cc','removed')
        command('pkg remove tinycc','removed')
        at=len(requests)
        command('pkg install cc','installed app cc')
        downloads=[path for path in requests[at:] if path.endswith('.elf')]
        assert len(downloads)==2 and '/tinycc/' in downloads[0] and '/cc/' in downloads[1],downloads
        command('pkg install jet','installed library jet')
        for m in base['modules']:
            directory='apps' if m['type']=='app' else 'libraries'
            assert sha(f'/modules/{directory}/{m["id"]}.elf')==m['artifact']['sha256']
        at=len(requests)
        command('pkg install cc','installed app cc')
        assert len([path for path in requests[at:] if path.endswith('.elf')])==1,'matching dependency downloaded again'
        # A corrupt download preserves the previous runnable compiler.
        cc=next(m for m in base['modules'] if m['id']=='cc')
        artifact=catalog_path.parent/cc['artifact']['path'];original=artifact.read_bytes()
        try:
            artifact.write_bytes(bytes([original[0]^1])+original[1:])
            command('pkg install cc','hash does not match')
            assert sha('/modules/apps/cc.elf')==cc['artifact']['sha256']
        finally:artifact.write_bytes(original)
        if not args.reuse_example:command('mkdir '+folder)
        command(f'curl -o {folder}/main.c http://{args.address}:{args.port}/main.c','curl: HTTP 200,')
        out=command('job start displayd display0','access code:')
        code=re.search(r'access code: (\d+)',out)[1];display_started=True
        editor=create('edit',folder+'/main.c')
        key(bytes([0xa8]));key(b'\n// Edited on SolarOS.\n');key(bytes([0x13]));key(bytes([0x11]))
        time.sleep(.5)
        assert not re.search(r'^'+editor+r'\s',command('session list'),re.M);sessions.remove(editor)
        source=remote(folder+'/main.c')
        assert b'// Edited on SolarOS.' in source
        (args.output/'edited-main.c').write_bytes(source)
        command('cd '+folder)
        command('cc main.c -o /modules/apps/jet-cube.elf','cc: /modules/apps/jet-cube.elf (')
        elf=remote('/modules/apps/jet-cube.elf');(args.output/'jet-cube.elf').write_bytes(elf)
        app=create('jet-cube','--paused')
        assert re.search(r'jet-cube\s+jet-cube\s+active',command('session list'))
        first=frame('cube')
        time.sleep(.5);assert frame('paused')==first,'paused frame changed'
        key(bytes([0x83]));rotated=frame('rotated');assert rotated!=first,'arrow did not rotate cube'
        command('pkg unload jet','pkg unload:')
        clock=create('clock')
        assert re.search(r'jet-cube\s+jet-cube\s+suspended',command('session list'))
        close(clock);time.sleep(.5)
        assert frame('resumed')==rotated,'suspended scene changed'
        key(b' ');time.sleep(1);assert frame('animated')!=rotated,'animation did not resume'
        key(b'q');time.sleep(.5)
        assert not re.search(r'^'+app+r'\s',command('session list'),re.M);sessions.remove(app)
        command('pkg unload jet','unloaded')
        command('pkg unload tinycc')
        memory=[command('mem')]
        for _ in range(3):
            app=create('jet-cube','--paused');close(app)
            command('pkg unload jet','unloaded');memory.append(command('mem'))
        managed=[re.search(r'Managed PSRAM: total (\d+) free (\d+) largest (\d+) resident (\d+)',m).groups() for m in memory]
        assert all(m==managed[0] for m in managed),'managed memory did not return to baseline'
        command('log show')
        report={'board':board,'ota':ota,'catalog':base,'requests':requests,
            'source_sha256':hashlib.sha256(source).hexdigest(),
            'output_sha256':hashlib.sha256(elf).hexdigest(),'output_bytes':len(elf),
            'checks':['service rejection before downloads','version rejection before downloads',
                'dependency-first installation','installed hashes','matching dependency reuse',
                'failed download preserves compiler','edit on device','compile on device',
                'visible cube','pause','arrow input','lease blocks unload','suspend/resume',
                'animation','Q exit','three reload/close/unload cycles','managed memory returns to baseline'], 'memory':memory}
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        print('Signed packages and on-device Jet C application: PASS',flush=True)
    finally:
        publish(base)
        for sid in reversed(sessions):
            try:command('session close '+sid)
            except Exception:pass
        if display_started:
            try:command('job stop displayd')
            except Exception:pass
        console.socket.close();log.close();server.shutdown();server.server_close()


if __name__=='__main__':main()
