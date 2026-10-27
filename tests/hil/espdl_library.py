#!/usr/bin/env python3
"""Exercise the ESP-DL resident library on a supplied S3 device.

Upload mode refuses pre-existing espdl modules and an active FTP job, and removes
only its own files. --installed repeats a staged probe without FTP or removal;
it also works with writerdeck. This tool does not build or flash firmware.
"""
import argparse
import ftplib
import hashlib
import json
from pathlib import Path
import re
import time
import uuid

from native_libraries import reconnect


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--telnet',required=True)
    parser.add_argument('--artifacts',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--installed',action='store_true')
    parser.add_argument('--probe',default='/sdcard/.espdl-check.app.elf')
    parser.add_argument('--ftp-port',type=int,default=2121)
    args=parser.parse_args()
    if not args.installed and not args.artifacts:parser.error('upload mode requires --artifacts')
    if not 1024<=args.ftp_port<=65535:parser.error('invalid FTP port')
    args.output.mkdir(parents=True,exist_ok=False)
    console=reconnect(args.telnet)
    ftp_started=False
    library_owned=False
    uploaded_probe=None
    with (args.output/'console.log').open('w') as log:
        def command(text,seconds=30):
            out=console.command(text,seconds);log.write(out);log.flush();print(out,flush=True);return out
        try:
            command('ota status');command('pkg installed')
            if not args.installed:
                report=json.loads((args.artifacts/'build.json').read_text())
                for name in ('espdl.library.elf','espdl-check.app.elf'):
                    if not (args.artifacts/name).is_file():raise RuntimeError('missing artifact '+name)
                    if hashlib.sha256((args.artifacts/name).read_bytes()).hexdigest()!=report['sha256'][name]:
                        raise RuntimeError('artifact differs from completed build: '+name)
                installed=command('pkg installed')
                if re.search(r'^library espdl\b',installed,re.M):raise RuntimeError('espdl already installed')
                if 'running' in command('job status ftpd').lower():raise RuntimeError('FTP job already running')
                out=command('job start ftpd /sdcard '+str(args.ftp_port))
                if 'job started: ftpd' not in out:raise RuntimeError('FTP job unavailable')
                ftp_started=True
                ftp=ftplib.FTP();ftp.connect(args.telnet,args.ftp_port);ftp.login()
                try:
                    for folder in ('/modules','/modules/libraries'):
                        try:ftp.mkd(folder)
                        except ftplib.error_perm:pass
                    library_owned=True
                    with (args.artifacts/'espdl.library.elf').open('rb') as data:
                        ftp.storbinary('STOR /modules/libraries/espdl.elf',data)
                    uploaded_probe='/.espdl-probe-'+uuid.uuid4().hex+'.app.elf'
                    with (args.artifacts/'espdl-check.app.elf').open('rb') as data:
                        ftp.storbinary('STOR '+uploaded_probe,data)
                finally:ftp.close()
                command('job stop ftpd');ftp_started=False
                args.probe='/sdcard'+uploaded_probe
            before=command('mem')
            output=command('load '+args.probe,90)
            if 'ESPDL_PROBE_OK cycles=15 runs=1125' not in output or 'FAILED' in output:
                raise RuntimeError('ESP-DL probe failed; inspect console.log')
            cycles=re.findall(r'ESPDL_CYCLE_OK (\w+) inference_us=(\d+) elapsed_us=(\d+)',output)
            if len(cycles)!=15:raise RuntimeError('missing cycle reports')
            memory=re.search(r'ESPDL_MEMORY internal=(\d+):(\d+) external=(\d+):(\d+)',output)
            if not memory:raise RuntimeError('missing memory report')
            counts=list(map(int,memory.groups()))
            if counts[0]-counts[1]>1024 or counts[2]-counts[3]>1024:
                raise RuntimeError('unexpected heap growth across unloaded cycles')
            after=command('mem')
            (args.output/'results.json').write_text(json.dumps({
                'cycles':15,'inferences':1125,'memory':counts,
                'timings':[{'model':name,'inference_us':int(t),'elapsed_us':int(e)} for name,t,e in cycles],
                'before':before,'after':after},indent=2)+'\n')
            print('ESPDL_LIBRARY_HIL_OK',flush=True)
        finally:
            if library_owned:command('pkg remove espdl')
            if uploaded_probe:
                if not ftp_started:
                    command('job start ftpd /sdcard '+str(args.ftp_port));ftp_started=True
                ftp=ftplib.FTP();ftp.connect(args.telnet,args.ftp_port);ftp.login()
                try:ftp.delete(uploaded_probe)
                finally:ftp.close()
            if ftp_started:command('job stop ftpd')
            console.socket.close()


if __name__=='__main__':main()
