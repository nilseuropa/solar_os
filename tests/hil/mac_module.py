#!/usr/bin/env python3
"""Boot an installed Mac module with owned assets and check retained frames.

Requires an idle display, installed mac.elf, and stopped displayd/ftpd jobs.
Saves the ROM's .pbm sidecar and guest disk writes on the device. No ROM or
system image is downloaded. The supplied system must boot to a desktop.
"""
import argparse
import ftplib
from pathlib import Path
import re
import shlex
import time
import urllib.request

from model_bundle_stream import Console


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--rom', required=True, help='Absolute device ROM path on /sdcard')
    media = parser.add_mutually_exclusive_group(required=True)
    media.add_argument('--floppy')
    media.add_argument('--scsi')
    parser.add_argument('--display', default='display0')
    parser.add_argument('--boot-wait', type=int, default=60)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--guest-size', type=int, nargs=2, metavar=('WIDTH', 'HEIGHT'),
                        help='Require native guest snapshots at this resolution')
    parser.add_argument('--check-fill', type=int, nargs=2, metavar=('WIDTH', 'HEIGHT'),
                        help='Compare an exported display frame (requires export of direct frames)')
    parser.add_argument('--check-native', action='store_true',
                        help='Require guest and display dimensions to match (--check-fill)')
    args = parser.parse_args()
    if not args.rom.startswith('/sdcard/') or not 10 <= args.boot_wait <= 600:
        parser.error('ROM must be on /sdcard; boot wait must be 10..600 seconds')
    if args.check_fill and any(not 1 <= size <= 1024 for size in args.check_fill):
        parser.error('Display dimensions must be 1..1024')
    if args.check_native and not args.check_fill:
        parser.error('--check-native requires --check-fill WIDTH HEIGHT')
    if args.guest_size and any(not 64 <= size <= 1024 for size in args.guest_size):
        parser.error('Guest dimensions must be 64..1024')
    args.output.mkdir(parents=True, exist_ok=True)
    console = Console(args.host)
    sessions = []
    display_started = ftp_started = False

    def command(*words):
        text = shlex.join(words)
        output = console.command(text, 10)
        print(text + '\n' + output, flush=True)
        return output

    def create(app, *arguments):
        out = command('session', 'create', app, args.display, *arguments)
        match = re.search(r'session (\d+) created', out)
        if not match:
            raise AssertionError(out)
        sessions.append(match[1])
        return match[1]

    def close(session):
        assert 'closed session' in command('session', 'close', session)
        sessions.remove(session)

    def key(*values):
        req = urllib.request.Request(f'http://{args.host}/api/displays/{args.display}/input',
            data=bytes(values), headers={'Authorization': 'Bearer ' + code})
        with urllib.request.urlopen(req, timeout=10) as response:
            assert response.status == 204
        time.sleep(1)

    def snapshot(name):
        nonlocal ftp_started
        assert re.search(r'\bmac\s+mac\s+active\b', command('session', 'list')), 'Mac session was closed during the test'
        key(0x87)  # F2: guest frame, including on native color display paths.
        out = command('job', 'start', 'ftpd', '/sdcard', '2121')
        if 'job started: ftpd' not in out:
            raise AssertionError(out)
        ftp_started = True
        with ftplib.FTP() as ftp:
            ftp.connect(args.host, 2121, timeout=10)
            ftp.login()
            with (args.output / name).open('wb') as stream:
                ftp.retrbinary('RETR ' + args.rom.removeprefix('/sdcard') + '.pbm', stream.write)
        command('job', 'stop', 'ftpd')
        ftp_started = False
        data = (args.output / name).read_bytes()
        width, height, _ = parse_pbm(data)
        if args.guest_size:
            assert (width, height) == tuple(args.guest_size), 'Unexpected guest resolution'
        return data

    def parse_pbm(data):
        match = re.match(rb'P4\n(\d+) (\d+)\n', data)
        assert match, 'Invalid PBM header'
        width, height = map(int, match.groups())
        assert len(data) - match.end() == ((width + 7) // 8) * height, 'Invalid PBM length'
        return width, height, data[match.end():]

    def check_fill(guest, name):
        if not args.check_fill:
            return
        width, height = args.check_fill
        req = urllib.request.Request(f'http://{args.host}/api/displays/{args.display}/frame.pbm',
            headers={'Authorization': 'Bearer ' + code})
        with urllib.request.urlopen(req, timeout=10) as response:
            actual = response.read()
        (args.output / name).write_bytes(actual)
        header = f'P4\n{width} {height}\n'.encode()
        stride = (width + 7) // 8
        expected = bytearray(stride * height)
        gw, gh, guest = parse_pbm(guest)
        if args.check_native:
            assert (gw, gh) == (width, height), 'Guest resolution differs from the display'
        guest_stride = (gw + 7) // 8
        for y in range(height):
            for x in range(width):
                sx, sy = x * gw // width, y * gh // height
                if guest[sy * guest_stride + sx // 8] & (0x80 >> (sx % 8)):
                    expected[y * stride + x // 8] |= 0x80 >> (x % 8)
        assert actual == header + expected, 'Display does not match the full-surface guest image'
        print(f'Full {width}x{height} display matches guest: PASS', flush=True)

    try:
        for job in ('displayd', 'ftpd'):
            if re.search(r'\brunning\b', command('job', 'status', job)):
                raise RuntimeError(f'{job} is already running; leave its existing owner in control')
        out = command('job', 'start', 'displayd', args.display)
        match = re.search(r'access code: (\d+)', out)
        assert match, out
        display_started = True
        code = match[1]
        guest_args = [args.rom, '--floppy', args.floppy] if args.floppy else [args.rom, args.scsi]
        mac = create('mac', *guest_args)
        deadline = time.monotonic() + args.boot_wait
        while time.monotonic() < deadline:
            time.sleep(min(10, max(0, deadline - time.monotonic())))
            print('Waiting for guest startup...', flush=True)
        key(0x8f)  # F10: freeze guest pixels, including its cursor.
        before = snapshot('before.pbm')
        # A desktop menu bar is predominantly white. RAM test, happy Mac,
        # missing-disk and welcome screens retain their gray pattern here.
        gw, gh, guest_pixels = parse_pbm(before)
        assert sum(byte.bit_count() for byte in guest_pixels[:12 * ((gw + 7) // 8)]) < gw * 12 // 4, 'Desktop menu bar absent'
        check_fill(before, 'display-before.pbm')
        for _ in range(3):
            clock = create('clock')
            state = command('session', 'list')
            assert re.search(r'\bmac\s+mac\s+suspended\b', state), state
            close(clock)
            state = command('session', 'list')
            assert re.search(r'\bmac\s+mac\s+active\b', state), state
        resumed = snapshot('resumed.pbm')
        assert resumed == before, 'Paused guest frame changed across suspension'
        check_fill(resumed, 'display-resumed.pbm')
        close(mac)
        probe = create('mac', '--self-test')
        time.sleep(2)
        close(probe)
        top = command('top')
        assert not re.search(r'^mac\s', top, re.M), top
        command('mem')
        print('Mac desktop, retained frame, suspend/resume and reload: PASS', flush=True)
    finally:
        for session in reversed(sessions):
            command('session', 'close', session)
        if ftp_started:
            command('job', 'stop', 'ftpd')
        if display_started:
            command('job', 'stop', 'displayd')
        console.socket.close()


if __name__ == '__main__':
    main()
