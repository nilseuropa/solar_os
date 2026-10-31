#!/usr/bin/env python3
"""Check finite PCM playback, cancellation, bindings and recovery on a device.

Requires Telnet, Python, SD and aplay. --lua also checks Lua. Creates only unique
silent WAV fixtures through the storage API, verifies their headers and sizes,
then removes them in finally. Does not change volume or persistent settings.
Do not run while another user owns the audio device.
"""
import argparse
import io
import json
from pathlib import Path
import re
import time
import wave
import uuid
from model_bundle_stream import Console
from bounded_repl import PythonRepl


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--lua', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    token = uuid.uuid4().hex[:8]
    for attempt in range(15):
        try:
            console = Console(args.host)
            break
        except (OSError, RuntimeError):
            if attempt == 14: raise
            time.sleep(.5)
    log = (args.output / 'console.log').open('w')
    installed, cases = [], []
    active = None

    def record(text):
        log.write(text + '\n'); log.flush()
        return text

    def command(text, seconds=30):
        output = record(text + '\n' + console.command(text, seconds))
        assert 'Guru Meditation' not in output and 'Traceback' not in output, output
        return output

    python = PythonRepl(lambda: console, record).command

    def enter_python():
        nonlocal active
        console.socket.sendall(b'python\r\n'); active = 'python'
        assert '>>> ' in record(console.read(2))
        python('import solaros; st=solaros.storage')

    def leave():
        nonlocal active
        console.socket.sendall(b'exit()\r\n' if active == 'python' else b'\x1d')
        record(console.read(2)); active = None

    try:
        for text in ('version', 'board', 'session list', 'audio status', 'audio devices', 'mem policy', 'top'):
            output = command(text)
            if text == 'session list':
                assert not re.search(r'^\d+\s+\S+\s+(?:aplay|arecord|synth|funcgen|webradio|vplay)\s', output, re.M)
        enter_python()
        for index, (rate, channels, duration) in enumerate((
            (16000, 1, .25), (16000, 2, .25), (48000, 1, 1.25), (48000, 2, .25), (48000, 1, 5),
        )):
            path = f'/sdcard/.review-{token}-{index}.wav'
            blob = io.BytesIO()
            with wave.open(blob, 'wb') as wav:
                wav.setnchannels(channels); wav.setsampwidth(2); wav.setframerate(rate)
                wav.writeframes(bytes(int(rate * duration) * channels * 2))
            header = blob.getvalue()[:44]
            size = len(blob.getvalue())
            python(f'p={path!r}')
            python('assert not st.exists(p)')
            installed.append(path)
            (args.output / 'fixtures.json').write_text(json.dumps(installed, indent=2) + '\n')
            python('st.write_file(p,' + repr(header) + ')')
            blocks, tail = divmod(size - 44, 16384)
            # Execute compound statements as a complete string; the interactive
            # editor would otherwise wait for a continuation line.
            python('exec(' + repr(f'for i in range({blocks}): st.write_file(p,bytes(16384),True)\n') + ')')
            if tail:
                python(f'st.write_file(p,bytes({tail}),True)')
            python(f"assert st.stat(p)['size']=={size}")
            python('assert st.read_file(p,44)==' + repr(header))
        python("print('PYTHON_STORAGE_OK')")
        leave()
        if args.lua:
            console.socket.sendall(b'lua\r\n'); active = 'lua'
            assert '> ' in record(console.read(2))
            for line in (f'p={installed[0]!r}', 'assert(solaros.storage.read_file(p,4)=="RIFF")', 'print("LUA_STORAGE_OK")'):
                assert len(line.encode()) < 72
                console.socket.sendall((line + '\r\n').encode())
                output = record(console.read(2))
                assert '> ' in output and 'error:' not in output.lower(), output
                if 'print(' in line: assert 'LUA_STORAGE_OK' in output
            leave()
        for path in installed[:4]:
            active = 'aplay'
            output = command('aplay ' + path)
            assert 'aplay: done' in output, output
            assert 'ESP_ERR_' not in output and 'failed' not in output.lower(), output
            active = None; cases.append(path)
        active = 'aplay'
        console.socket.sendall(('aplay ' + installed[4] + '\r\n').encode())
        output = record(console.read(.7))
        assert 'playing ' in output, output
        console.socket.sendall(b'\x1d')
        output = record(console.read(15, True))
        assert re.search(r'\w+@[^\r\n]+:/[^\r\n]* ', output), output
        active = None
        for _ in range(3):
            command('mem reclaim')
            output = command('mem policy')
            match = re.search(r'Recovery: last (\d+) us max (\d+) us; compactions last (\d+) total (\d+)', output)
            assert match and 1 <= int(match[3]) <= 2 and int(match[2]) >= int(match[1]), output
        # Complete an option, then clear the owned line without executing it.
        console.socket.sendall(b'xfer send uart0 fixture --z\t')
        output = record(console.read(2))
        assert '--zmodem' in output, output
        console.socket.sendall(b'\x03'); record(console.read(2))
        for text in ('session list', 'audio status', 'mem policy', 'top', 'log show'):
            command(text)
    finally:
        if active:
            leave()
        if installed:
            enter_python()
            for path in installed:
                python(f'p={path!r}')
                python('st.remove(p) if st.exists(p) else None')
                python('assert not st.exists(p)')
            leave()
        log.close(); console.socket.close()
    (args.output / 'result.json').write_text(json.dumps(dict(host=args.host, wav_cases=len(cases),
        cancellation='returned to shell', python='pass', lua='pass' if args.lua else 'not selected',
        recovery='pass', completion='pass', fixtures_removed=True), indent=2) + '\n')
    print('RUNTIME_STAGE_OK', args.host, flush=True)


if __name__ == '__main__':
    main()
