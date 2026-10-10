#!/usr/bin/env python3
"""Validate the imlib subset on a device with Telnet, FTP, Python and Lua.

Uploads only absent .imlib-* temporary fixtures; cleans its files and FTP job.
Run after flashing a build containing service.imlib. No camera/model required.
"""
import argparse
import ftplib
import io
from pathlib import Path
import re
import struct
import time
import zlib
from model_bundle_stream import Console

ROOT = Path(__file__).resolve().parents[2]


def png(width, height, pixels):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + pixels[y*width*3:(y+1)*width*3] for y in range(height))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def fixtures():
    scene = bytearray([20] * (32*24*3))
    color = bytearray(32*24*3)
    for y in range(6, 12):
        for x in range(8, 16):
            scene[(y*32+x)*3:(y*32+x)*3+3] = bytes([200]*3)
            color[(y*32+x)*3] = 255
    scene[(2*32+2)*3:(2*32+2)*3+3] = bytes([200]*3)
    noise = bytearray(9*9*3)
    noise[(4*9+4)*3:(4*9+4)*3+3] = bytes([255]*3)
    large = bytes((x*13 + y*17) % 256 for y in range(480) for x in range(640) for _ in range(3))
    files = {
        '.imlib-scene.png': png(32, 24, scene),
        '.imlib-reference.png': png(32, 24, bytes([20] * (32*24*3))),
        '.imlib-color.png': png(32, 24, color),
        '.imlib-noise.png': png(9, 9, noise),
        '.imlib-large.png': png(640, 480, large),
    }
    for name in ('imlib_client.py', 'imlib_client.lua', 'imlib_cancel.py'):
        files['.' + name.replace('_', '-')] = (ROOT/'tests/hil'/name).read_bytes()
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--telnet', required=True)
    parser.add_argument('--ftp-port', type=int, default=2121)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    console = Console(args.telnet)
    uploaded = []
    ftp_started = script_running = False
    log = (args.output/'console.log').open('w')

    def command(text, seconds=20):
        out = console.command(text, seconds)
        log.write(out); log.flush(); print(out, flush=True)
        return out

    def ftp():
        client = ftplib.FTP()
        client.connect(args.telnet, args.ftp_port, timeout=10); client.login()
        client.voidcmd('TYPE I')
        return client

    try:
        status = command('job status')
        if re.search(r'ftpd[^\r\n]*(running|RUNNING)', status):
            raise RuntimeError('Stop the existing FTP job before this test')
        command('job start ftpd /dl %d' % args.ftp_port); ftp_started = True
        with ftp() as client:
            for name, data in fixtures().items():
                try:
                    client.size('/'+name)
                except ftplib.error_perm as error:
                    if not str(error).startswith('550'): raise
                else:
                    raise RuntimeError('Refusing to overwrite '+name)
                client.storbinary('STOR /'+name, io.BytesIO(data)); uploaded.append(name)
        command('job stop ftpd'); ftp_started = False
        (args.output/'before.log').write_text(command('mem') + command('top'))
        output = command('python /dl/.imlib-client.py', 45)
        (args.output/'python.log').write_text(output)
        assert 'IMLIB_PYTHON_OK' in output, 'Python subset validation failed'
        output = command('lua /dl/.imlib-client.lua', 45)
        (args.output/'lua.log').write_text(output)
        assert 'IMLIB_LUA_OK' in output, 'Lua subset validation failed'
        console.socket.sendall(b'python /dl/.imlib-cancel.py\r\n'); script_running = True
        output = console.read(1.0)
        assert 'IMLIB_CANCEL_READY' in output, 'Cancellation loop did not start'
        time.sleep(.05)
        console.socket.sendall(b'\x1d')
        output += console.read(15, True)
        log.write(output); log.flush(); print(output, flush=True)
        (args.output/'cancel.log').write_text(output)
        assert re.search(r'\w+@[^\r\n]+:/', output), 'No shell after cancellation'
        script_running = False
        after = command('mem') + command('top')
        (args.output/'after.log').write_text(after)
        assert not re.search(r'^vision-imlib\s', after, re.M), 'imlib worker remained after cancellation'
        print('IMLIB_SUBSET_OK: Python/Lua, six groups, source ownership, repeated calls and cancellation')
    finally:
        try:
            if script_running:
                console.socket.sendall(b'\x1d'); console.read(10, True)
            if uploaded:
                if not ftp_started:
                    command('job start ftpd /dl %d' % args.ftp_port); ftp_started = True
                with ftp() as client:
                    for name in uploaded: client.delete('/'+name)
            if ftp_started: command('job stop ftpd')
        finally:
            console.socket.close(); log.close()


if __name__ == '__main__':
    main()
