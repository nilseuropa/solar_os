#!/usr/bin/env python3
"""Compare real models through the firmware frontend across provider flavors.

Stage a probe and explicitly supplied models in an owned unique SD directory.
--installed repeats the paths recorded by --keep-fixtures after a flavor change.
--cleanup removes those paths. No user model handles or files are modified.
"""
import argparse
import ftplib
import json
from pathlib import Path
import re
import uuid
from native_libraries import reconnect


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--telnet',required=True)
    p.add_argument('--probe',type=Path)
    p.add_argument('--model',type=Path,action='append',default=[])
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--installed',type=Path,help='previous fixtures.json')
    p.add_argument('--expected',type=Path,help='baseline results.json')
    p.add_argument('--keep-fixtures',action='store_true')
    p.add_argument('--cleanup',action='store_true')
    a=p.parse_args()
    if not a.installed and (not a.probe or not a.model):p.error('stage mode needs --probe and --model')
    if a.cleanup and not a.installed:p.error('cleanup needs --installed')
    a.output.mkdir(parents=True,exist_ok=False)
    fixtures=json.loads(a.installed.read_text()) if a.installed else {
        'directory':'/.espdl-vision-'+uuid.uuid4().hex,
        'models':[str(i)+'.espdl' for i in range(len(a.model))], 'probe':'check.app.elf'}
    directory=fixtures['directory']
    if not re.fullmatch(r'/\.espdl-vision-[0-9a-f]{32}',directory) or fixtures['probe']!='check.app.elf' or any(
        not re.fullmatch(r'\d+\.espdl',name) for name in fixtures['models']):raise RuntimeError('invalid fixture paths')
    c=reconnect(a.telnet);ftp_started=False;owned=False
    log=(a.output/'console.log').open('w')
    def cmd(text,seconds=30):
        out=c.command(text,seconds);log.write(out);log.flush();print(out,flush=True);return out
    def ftp_open():
        nonlocal ftp_started
        if 'running' in cmd('job status ftpd').lower():raise RuntimeError('FTP already running')
        if 'job started: ftpd' not in cmd('job start ftpd /sdcard 2121'):raise RuntimeError('FTP unavailable')
        ftp_started=True;f=ftplib.FTP();f.connect(a.telnet,2121);f.login();return f
    def remove():
        nonlocal ftp_started
        f=ftp_open()
        try:
            for name in [fixtures['probe'],*fixtures['models']]:f.delete(directory+'/'+name)
            f.rmd(directory)
        finally:f.close();cmd('job stop ftpd');ftp_started=False
    try:
        cmd('ota status');cmd('pkg installed')
        if a.cleanup:remove();return
        if not a.installed:
            f=ftp_open()
            try:
                f.mkd(directory);owned=True
                for name,source in [(fixtures['probe'],a.probe),*zip(fixtures['models'],a.model)]:
                    with source.open('rb') as stream:f.storbinary('STOR '+directory+'/'+name,stream)
            finally:f.close();cmd('job stop ftpd');ftp_started=False
            (a.output/'fixtures.json').write_text(json.dumps(fixtures,indent=2)+'\n')
        baseline=json.loads(a.expected.read_text()) if a.expected else None
        results=[]
        for index,name in enumerate(fixtures['models']):
            out=cmd('load /sdcard'+directory+'/'+fixtures['probe']+' /sdcard'+directory+'/'+name,180)
            ok=re.search(r'VISION_OK rounds=3 runs=21 hashes=([0-9a-f]+):([0-9a-f]+)',out)
            if not ok or 'FAILED' in out or 'MISMATCH' in out:raise RuntimeError('vision probe failed')
            hashes=list(ok.groups())
            if baseline and hashes!=baseline[index]['hashes']:raise RuntimeError('provider differs from baseline')
            memory=re.search(r'VISION_MEMORY internal=(\d+):(\d+) external=(\d+):(\d+)',out)
            if not memory:raise RuntimeError('missing memory results')
            heap=list(map(int,memory.groups()))
            if heap[0]-heap[1]>1024 or heap[2]-heap[3]>1024:raise RuntimeError('heap loss')
            runs=re.findall(r'VISION_RUN round=(\d+) mode=(\d+) seed=(\d+) hash=([0-9a-f]+) inference_us=(\d+) elapsed_us=(\d+)',out)
            if len(runs)!=18:raise RuntimeError('missing inference results')
            working_sets=re.findall(r'VISION_RUN.*?mode=(\d+).*?resident_internal=(\d+) resident_external=(\d+)',out)
            results.append({'model':name,'hashes':hashes,'memory':heap,'runs':runs,'working_sets':working_sets})
        (a.output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print('INFERENCE_BACKENDS_OK',flush=True)
    finally:
        if owned and not a.keep_fixtures:remove()
        if ftp_started:cmd('job stop ftpd')
        log.close();c.socket.close()


if __name__=='__main__':main()
