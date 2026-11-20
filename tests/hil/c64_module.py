#!/usr/bin/env python3
"""Install and check the C64 module's original diagnostic guest on an idle display.

Requires stopped displayd/ftpd jobs. Leaves the ELF and diagnostic files on SD.
No Commodore ROMs or games are needed. Restores sessions and jobs on exit.
"""
import argparse
import ftplib
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import time
import urllib.request

from model_bundle_stream import Console


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--elf', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--require-audio', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    console = Console(args.host)
    sessions, jobs, log, records = [], set(), [], {}

    def command(text):
        output = console.command(text, 10)
        log.append(text + '\n' + output)
        print(text + '\n' + output, flush=True)
        return output

    def start(job, arguments):
        output = command(f'job start {job} {arguments}')
        if f'job waiting: {job}' in output or f'job started: {job}' in output:
            jobs.add(job)
        assert f'job started: {job}' in output, output
        return output

    def stop(job):
        assert f'job stopped: {job}' in command(f'job stop {job}')
        jobs.remove(job)

    def create(app, arguments=''):
        output = command(f'session create {app} display0 {arguments}')
        match = re.search(r'session (\d+) created', output)
        assert match, output
        sessions.append(match[1])
        return match[1]

    def close(session):
        assert 'closed session' in command(f'session close {session}')
        sessions.remove(session)

    def ftp(action):
        start('ftpd', '/sdcard 2121')
        try:
            with ftplib.FTP() as connection:
                connection.connect(args.host, 2121, timeout=15)
                connection.login()
                return action(connection)
        finally:
            stop('ftpd')

    def key(*values):
        request = urllib.request.Request(f'http://{args.host}/api/displays/display0/input',
            data=bytes(values), headers={'Authorization': 'Bearer ' + code})
        with urllib.request.urlopen(request, timeout=10) as response:
            assert response.status == 204

    def snapshot(label, *keys):
        key(*keys, 0x87)  # Capture after a guest slice, before logical taps expire.
        time.sleep(.5)
        # Free foreground audio's internal stack before opening an FTP worker.
        helper = create('clock')
        try:
            def retrieve(connection):
                result = []
                for suffix in ('.ppm', '.ppm.state'):
                    out = io.BytesIO()
                    connection.retrbinary('RETR /c64/diagnostic' + suffix, out.write)
                    (args.output / (label + suffix)).write_bytes(out.getvalue())
                    result.append(out.getvalue())
                return result
            picture, state = ftp(retrieve)
        finally:
            close(helper)
        assert picture.startswith(b'P6\n320 200\n255\n') and len(picture) == 192015
        assert len(state) == 80 and state[:4] == b'C64S'
        version, frames, pc, counter, paused, audio, port = struct.unpack_from('<7I', state, 4)
        elapsed, presented, replaced, present_us = struct.unpack_from('<IIIQ', state, 44)
        assert version == 1 and frames > 0 and 0xe000 <= pc < 0xe080 and presented > 0
        record = dict(frames=frames, pc=hex(pc), counter=counter, paused=paused,
            audio=audio, keyboard_port=port, matrix=list(state[32:40]),
            joystick=list(state[40:42]), guest_cia=list(state[42:44]),
            elapsed_ms=elapsed, presented=presented, replaced=replaced, present_us=present_us)
        records[label] = record
        print(label, record, flush=True)
        return picture, record

    try:
        for job in ('displayd', 'ftpd'):
            assert not re.search(r'\brunning\b', command(f'job status {job}')), f'{job} already running'
        assert not re.search(r'\bc64\s+c64\s', command('session list')), 'C64 already loaded'
        for text in ('version', 'board', 'display list', 'mem', 'audio status'):
            command(text)
        command('mkdir /sdcard/c64')
        command('mkdir /sdcard/modules/apps')
        payload = args.elf.read_bytes()

        def install(connection):
            # Preserve an existing module before replacing it.
            try:
                connection.size('/modules/apps/c64.elf')
            except ftplib.error_perm as error:
                if not str(error).startswith('550'):
                    raise
            else:
                previous = io.BytesIO()
                connection.retrbinary('RETR /modules/apps/c64.elf', previous.write)
                (args.output / 'previous-c64.elf').write_bytes(previous.getvalue())
            connection.storbinary('STOR /modules/apps/c64.elf', io.BytesIO(payload))
            verify = io.BytesIO()
            connection.retrbinary('RETR /modules/apps/c64.elf', verify.write)
            assert payload == verify.getvalue()
        ftp(install)
        records['install'] = dict(size=len(payload), sha256=hashlib.sha256(payload).hexdigest())
        output = start('displayd', 'display0')
        match = re.search(r'access code: (\d+)', output)
        assert match, output
        code = match[1]
        guest = create('c64', '--self-test /sdcard/c64/diagnostic.ppm')
        time.sleep(2)
        first, a = snapshot('running')
        time.sleep(1)
        second, b = snapshot('advanced')
        assert b['frames'] > a['frames'] and second != first
        # Host presenter statistics restart when the transfer helper suspends
        # and resumes the guest. Each snapshot must show completed transfers.
        assert a['presented'] and b['presented']
        if args.require_audio:
            assert a['audio'] and b['audio']
            assert 'owner c64' in command('audio status')
        _, keyboard = snapshot('keyboard', ord('a'))
        assert keyboard['matrix'][1] & 4 == 0, 'A key did not reach the CIA matrix'
        assert keyboard['guest_cia'][0] & 4 == 0, 'Guest did not read the A key'
        _, joystick = snapshot('joystick', 0x8e, 0x83)  # F9, Right
        assert joystick['keyboard_port'] == 2 and joystick['joystick'][1] & 8 == 0
        assert joystick['guest_cia'][1] & 8 == 0, 'Guest did not read joystick Right'
        _, neutral = snapshot('released')
        assert neutral['joystick'] == [255, 255]
        retained, p = snapshot('paused', 0x8f)  # F10
        assert p['paused'] and not p['audio']
        for _ in range(3):
            helper = create('clock')
            assert re.search(r'\bc64\s+c64\s+suspended\b', command('session list'))
            close(helper)
            assert re.search(r'\bc64\s+c64\s+active\b', command('session list'))
        resumed, r = snapshot('resumed')
        assert resumed == retained and r['frames'] == p['frames'] and r['counter'] == p['counter']
        assert r['joystick'] == [255, 255] and r['matrix'] == [255] * 8
        key(0x8f)
        time.sleep(1)
        _, running = snapshot('continued')
        assert running['frames'] > r['frames'] and not running['paused']
        if args.require_audio:
            assert running['audio']
        close(guest)
        for _ in range(3):
            guest = create('c64', '--self-test')
            time.sleep(1)
            close(guest)
        command('mem')
        assert not re.search(r'^c64\s', command('top'), re.M)
        assert 'Synth: idle' in command('audio status')
        print('C64_HIL_OK: ELF read-back, CPU/video, keyboard/joystick, pause, retained '
              'suspend/resume, audio ownership, close and relaunch', flush=True)
    finally:
        for session in reversed(sessions):
            command(f'session close {session}')
        for job in tuple(jobs):
            command(f'job stop {job}')
        (args.output / 'console.log').write_text('\n'.join(log))
        (args.output / 'results.json').write_text(json.dumps(records, indent=2) + '\n')
        console.socket.close()


if __name__ == '__main__':
    main()
