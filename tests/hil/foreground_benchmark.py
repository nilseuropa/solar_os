#!/usr/bin/env python3
"""Build and run the finite foreground service/presenter benchmark.

Temporarily installs fgbench through normal display session switching, and
restores the existing FTP listener. Saves timing rows and the console log.
"""
import argparse
import ftplib
import io
from pathlib import Path
import re
import subprocess
import tempfile
import time

from model_bundle_stream import Console

ROOT = Path(__file__).resolve().parents[2]


def build(output):
    compiler = Path.home() / '.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc'
    subprocess.run([str(compiler), '-O2', '-g0', '-mlongcalls', '-fPIC', '-fvisibility=hidden',
                    '-ffunction-sections', '-fdata-sections', '-Dmain=app_main', '-I', str(ROOT / 'include'),
                    '-nostdlib', '-nostartfiles', '-shared', '-Wl,-e,app_main',
                    '-Wl,--undefined=solar_os_native_app_manifest', '-Wl,--gc-sections',
                    str(ROOT / 'tests/hil/fixtures/foreground_bench.c'), '-lgcc', '-o', str(output)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    for attempt in range(5):
        try:
            console = Console(args.host)
            break
        except (OSError, RuntimeError):
            if attempt == 4:
                raise
            time.sleep(1)
    log = []

    def command(text):
        out = console.command(text, 10)
        log.append(text + '\n' + out)
        print(text + '\n' + out, flush=True)
        return out

    status = command('job status ftpd')
    match = re.search(r'tcp:(\d+) listen', status)
    original_port = int(match[1]) if match else None
    if original_port is not None and not re.search(r'- file\s+/sdcard\s+export', status):
        console.socket.close()
        raise RuntimeError('The existing FTP listener must export /sdcard')
    ftp_running = original_port is not None
    session = None
    installed = False
    try:
        command('version'); command('power status'); command('session list')
        if not ftp_running:
            assert 'job started' in command('job start ftpd /sdcard 2121')
            ftp_running = True
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / 'fgbench.elf'
            build(elf)
            with ftplib.FTP() as ftp:
                ftp.connect(args.host, original_port or 2121, timeout=15); ftp.login()
                try:
                    ftp.size('/modules/apps/fgbench.elf')
                except ftplib.error_perm as error:
                    if not str(error).startswith('550'):
                        raise
                else:
                    raise RuntimeError('fgbench.elf already exists; refusing to overwrite it')
                blob = elf.read_bytes()
                ftp.storbinary('STOR /modules/apps/fgbench.elf', io.BytesIO(blob))
                installed = True
                result = io.BytesIO(); ftp.retrbinary('RETR /modules/apps/fgbench.elf', result.write)
                assert blob == result.getvalue()
        command('job stop ftpd'); ftp_running = False
        command('rm -f /sdcard/.fgbench-result.txt')
        out = command('session create fgbench display0 /sdcard/.fgbench-result.txt')
        match = re.search(r'session (\d+) created', out)
        assert match, out
        session = match[1]
        for _ in range(30):
            time.sleep(1)
            out = command('cat /sdcard/.fgbench-result.txt')
            match = re.search(r'FGBENCH1\s*\n.*?\bEND\b', out, re.S)
            if match:
                rows = {parts[0]: [int(value) for value in parts[1:]]
                        for line in match[0].splitlines()
                        if len(parts := line.split()) == 4 and parts[0] != 'presented'}
                assert rows['kernel'][0] == 500000 and rows['kernel'][2] == 1797929924
                assert rows['dispatch'][0] == 50000 and rows['dispatch'][2] == 1903286409
                for name, checksum in (('ram8k', 1898964316), ('ram32k', 2978946024), ('ram128k', 1653526954)):
                    assert rows[name][0] == 50000 and rows[name][2] == checksum, (name, rows[name])
                for name in ('burst', 'paced', 'audio', 'exchange_burst', 'exchange_paced', 'exchange_audio'):
                    if name in rows:
                        assert rows[name][0] == 60, (name, rows[name])
                assert 'burst' in rows and 'paced' in rows
                args.output.write_text(match[0] + '\n')
                break
        else:
            raise AssertionError('Benchmark did not finish')
        command('session list'); command('mem')
        print('FOREGROUND_BENCH_OK', flush=True)
    finally:
        if session:
            out = command('session list')
            if re.search(r'^' + session + r'\s+fgbench\s+fgbench\s', out, re.M):
                command('session close ' + session)
        if installed:
            command('rm -f /sdcard/modules/apps/fgbench.elf /sdcard/.fgbench-result.txt')
        if ftp_running and original_port is None:
            command('job stop ftpd')
        elif not ftp_running and original_port is not None:
            command(f'job start ftpd /sdcard {original_port}')
        args.output.with_suffix('.log').write_text('\n'.join(log))
        console.socket.close()


if __name__ == '__main__':
    main()
