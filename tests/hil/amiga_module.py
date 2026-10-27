#!/usr/bin/env python3
"""Install and exercise amiga's original diagnostic guest on an idle display.

No Kickstart or disk images are needed. Requires stopped displayd/ftpd jobs.
The installed module and two diagnostic snapshot files remain on storage.
"""
import argparse
import ftplib
import hashlib
import io
import re
import struct
import time
import urllib.request
from pathlib import Path

from model_bundle_stream import Console


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--size', type=int, nargs=2, required=True, metavar=('WIDTH', 'HEIGHT'))
    parser.add_argument('--require-audio', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    console = Console(args.host)
    sessions = []
    started = set()
    log = []

    def command(text):
        out = console.command(text, 10)
        log.append(text + '\n' + out)
        print(text + '\n' + out, flush=True)
        return out

    def start_job(job, arguments):
        out = command(f'job start {job} {arguments}')
        if f'job waiting: {job}' in out:
            started.add(job)
        assert f'job started: {job}' in out, out
        started.add(job)
        return out

    def stop_job(job):
        command(f'job stop {job}')
        started.remove(job)

    def ftp_transfer(action, quiesce=False):
        # Audio and FTP worker stacks together can exceed a board's internal
        # reserve. The snapshot is already on storage; suspend during download.
        clock = create('clock') if quiesce else None
        try:
            start_job('ftpd', '/sdcard 2121')
            with ftplib.FTP() as ftp:
                ftp.connect(args.host, 2121, timeout=15)
                ftp.login()
                return action(ftp)
        finally:
            if 'ftpd' in started:
                stop_job('ftpd')
            if clock is not None:
                close(clock)

    def create(app, arguments=''):
        out = command(f'session create {app} display0 {arguments}')
        match = re.search(r'session (\d+) created', out)
        assert match, out
        sessions.append(match[1])
        return match[1]

    def close(session):
        assert 'closed session' in command(f'session close {session}')
        sessions.remove(session)

    def key(*values):
        request = urllib.request.Request(f'http://{args.host}/api/displays/display0/input',
            data=bytes(values), headers={'Authorization': 'Bearer ' + code})
        with urllib.request.urlopen(request, timeout=10) as response:
            assert response.status == 204
        time.sleep(.5)

    def snapshot(label):
        key(0x87)
        def retrieve(ftp):
            result = []
            for suffix in ('.ppm', '.ppm.state'):
                data = io.BytesIO()
                ftp.retrbinary('RETR /amiga/diagnostic' + suffix, data.write)
                (args.output / (label + suffix)).write_bytes(data.getvalue())
                result.append(data.getvalue())
            return result
        picture, state = ftp_transfer(retrieve, quiesce=True)
        width, height = args.size
        header = f'P6\n{width} {height}\n255\n'.encode()
        assert picture.startswith(header) and len(picture) == len(header) + width * height * 3
        assert len(state) == 32 and state[:4] == b'AMGA'
        frame, pc, counter, joy, mouse = struct.unpack_from('>IIHHH', state, 4)
        assert frame > 0 and 0xfc0010 <= pc < 0xfc0040 and counter
        assert state[23] and struct.unpack_from('>I', state, 24)[0] > 0, 'No completed display presentations'
        record = dict(frame=frame, pc=hex(pc), counter=counter, joystick=joy, mouse=mouse,
                      left=state[18], right=state[19], paused=state[20], audio=state[21], mouse_mode=state[22])
        print(label, record, flush=True)
        return picture, record

    try:
        for job in ('displayd', 'ftpd'):
            assert not re.search(r'\brunning\b', command(f'job status {job}')), f'{job} already running'
        assert not re.search(r'\bamiga\s+amiga\s', command('session list')), 'amiga is already loaded'
        command('mem')
        command('mkdir /sdcard/amiga')
        command('mkdir /sdcard/modules/apps')
        payload = args.elf.read_bytes()

        def install(ftp):
            ftp.storbinary('STOR /modules/apps/amiga.elf', io.BytesIO(payload))
            verify = io.BytesIO()
            ftp.retrbinary('RETR /modules/apps/amiga.elf', verify.write)
            assert hashlib.sha256(payload).digest() == hashlib.sha256(verify.getvalue()).digest()
        ftp_transfer(install)
        out = start_job('displayd', 'display0')
        match = re.search(r'access code: (\d+)', out)
        assert match, out
        code = match[1]
        amiga = create('amiga', '--self-test /sdcard/amiga/diagnostic.ppm')
        time.sleep(2)
        first, a = snapshot('running')
        if args.require_audio:
            assert a['audio'], 'Host audio did not start'
        time.sleep(1)
        second, b = snapshot('advanced')
        assert b['frame'] > a['frame'] and b['counter'] != a['counter'] and second != first
        key(0x8f)  # pause
        paused, p = snapshot('paused')
        assert p['paused'] and not p['audio']
        key(0x8c)  # F7: mouse mode
        key(0x83, 0x87)  # mouse right and snapshot before pulse expires
        _, moved = snapshot('mouse')
        assert moved['mouse'] != p['mouse']
        key(0x8d, 0x83, 0x87)  # F8: joystick mode, right, snapshot before pulse expires
        def joystick(ftp):
            data = io.BytesIO()
            ftp.retrbinary('RETR /amiga/diagnostic.ppm.state', data.write)
            return data.getvalue()
        state = ftp_transfer(joystick, quiesce=True)
        assert struct.unpack_from('>H', state, 14)[0] == 3 and not state[22]
        retained, retained_state = snapshot('retained')
        for _ in range(3):
            clock = create('clock')
            assert re.search(r'\bamiga\s+amiga\s+suspended\b', command('session list'))
            close(clock)
            assert re.search(r'\bamiga\s+amiga\s+active\b', command('session list'))
        resumed, r = snapshot('resumed')
        assert resumed == retained and r['frame'] == retained_state['frame'] and r['counter'] == retained_state['counter']
        assert r['joystick'] == 0
        key(0x8f)
        time.sleep(1)
        _, running = snapshot('continued')
        assert running['frame'] > r['frame'] and not running['paused']
        if args.require_audio:
            assert running['audio'], 'Host audio did not restart after pause'
        close(amiga)
        probe = create('amiga', '--self-test')
        time.sleep(1)
        close(probe)
        command('mem')
        assert not re.search(r'^amiga\s', command('top'), re.M)
        print('AMIGA_HIL_OK: verified ELF install, CPU and rendered frames, mouse/joystick input, pause, retained suspend/resume, close and relaunch', flush=True)
    finally:
        for session in reversed(sessions):
            command(f'session close {session}')
        for job in tuple(started):
            stop_job(job)
        (args.output / 'console.log').write_text('\n'.join(log))
        console.socket.close()


if __name__ == '__main__':
    main()
