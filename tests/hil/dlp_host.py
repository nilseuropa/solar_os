#!/usr/bin/env python3
"""Exercise SolarOS's DLP host against a Pico USB endpoint via an owned TCP bridge.

Requires an idle Pico serial port, SolarOS telnet console, and stopped FTP job.
Uploads temporary Python/Lua probes to the device SD card, cleans them and its
streams/attachments, and leaves the final probe frame on the Pico display.
The USB bridge tests stream-dlp and private clients, not physical UART wiring.
"""
import argparse
import ftplib
import io
import json
from pathlib import Path
import re
import select
import socket
import threading

import serial
from model_bundle_stream import Console


class Bridge:
    def __init__(self, port, device):
        self.serial = serial.Serial(device, 115200, timeout=0, write_timeout=2)
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(('0.0.0.0', port))
        self.listener.listen(1)
        self.listener.settimeout(.1)
        self.stop = threading.Event()
        self.errors = []
        self.count = self.tx = self.rx = 0
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            while not self.stop.is_set():
                try:
                    peer, _ = self.listener.accept()
                except socket.timeout:
                    continue
                self.count += 1
                with peer:
                    peer.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    peer.settimeout(2)
                    self.serial.reset_input_buffer()
                    while not self.stop.is_set():
                        ready, _, _ = select.select([peer, self.serial], [], [], .05)
                        if peer in ready:
                            data = peer.recv(4096)
                            if not data:
                                break
                            at = 0
                            while at < len(data):
                                at += self.serial.write(data[at:])
                            self.tx += len(data)
                        if self.serial in ready:
                            data = self.serial.read(4096)
                            if data:
                                peer.sendall(data)
                                self.rx += len(data)
        except Exception as error:
            if not self.stop.is_set():
                self.errors.append(repr(error))

    def close(self):
        self.stop.set()
        self.thread.join(3)
        self.listener.close()
        self.serial.close()
        assert not self.thread.is_alive(), 'bridge did not stop'
        assert not self.errors, self.errors


def probes(address, port):
    python = '''import solaros, gc
name = "dlp-py-hil"
solaros.streams.tcp.connect(name, ADDRESS, PORT)
first = solaros.dlp.connect(name, timeout_ms=3000)
info = first.info()
assert info["width"] == 240 and info["height"] == 64
assert info["format"] == solaros.dlp.MONO1 and info["frame_bytes"] == 1920
try:
    first.present(b"bad")
    raise AssertionError("accepted short frame")
except OSError:
    pass
first.present(bytes(info["frame_bytes"]))
first.close()
second = solaros.dlp.connect(name)
first.close()  # an old object must not close a reused slot
second.present(bytes(info["frame_bytes"]))
try:
    first.info()
    raise AssertionError("accepted closed object")
except ValueError:
    pass
second.close()
third = solaros.dlp.connect(name)
del third
gc.collect()
with solaros.dlp.connect(name) as scoped:
    scoped.present(bytes(info["frame_bytes"]))
# Deliberately leave this connection open: interpreter teardown must release it.
last = solaros.dlp.connect(name)
last.present(bytes(info["frame_bytes"]))
print("DLP_PY_PASS", info)
'''.replace('ADDRESS', json.dumps(address)).replace('PORT', str(port))
    lua = '''local solaros = require("solaros")
local name = "dlp-lua-hil"
solaros.streams.tcp.connect(name, ADDRESS, PORT)
local first = solaros.dlp.connect(name, solaros.dlp.MONO1, 3000)
local info = first:info()
assert(info.width == 240 and info.height == 64 and info.frame_bytes == 1920)
assert(not pcall(function() first:present("bad") end))
first:present(string.rep(string.char(0), info.frame_bytes))
first:close()
local second = solaros.dlp.connect(name)
first:close()
second:present(string.rep(string.char(0), info.frame_bytes))
assert(not pcall(function() first:info() end))
second:close()
do
    local third = solaros.dlp.connect(name)
end
collectgarbage("collect")
do
    local scoped <close> = solaros.dlp.connect(name)
    scoped:present(string.rep(string.char(0), info.frame_bytes))
end
local last = solaros.dlp.connect(name)
local rows = {}
for y = 0, info.height - 1 do
    local row = {}
    for b = 0, info.stride - 1 do
        local v = 0
        for bit = 0, 7 do
            local x = b * 8 + bit
            if y == 0 or y == info.height - 1 or x == 0 or x == info.width - 1 or
                (x >= 16 and x < 224 and y >= 16 and y < 48 and
                 ((x // 16 + y // 8) % 2 == 0)) then
                v = v | (1 << bit)
            end
        end
        row[#row + 1] = string.char(v)
    end
    rows[#rows + 1] = table.concat(row)
end
last:present(table.concat(rows))
print("DLP_LUA_PASS", info.width, info.height)
'''.replace('ADDRESS', json.dumps(address)).replace('PORT', str(port))
    named_python = '''import solaros
name = "dlp-py-hil"
target = "py-dlp-hil"
solaros.streams.tcp.connect(name, ADDRESS, PORT)
solaros.expansion.attach("stream-dlp", target, {"stream": name, "timeout-ms": 3000})
try:
    solaros.gfx.begin(target)
    try:
        try:
            solaros.expansion.detach(target)
            raise AssertionError("detached a claimed display")
        except OSError:
            pass
        solaros.gfx.clear(solaros.gfx.WHITE)
        solaros.gfx.present()
    finally:
        solaros.gfx.end()
finally:
    # gfx.end() queues its release on the foreground UI task.
    for attempt in range(250):
        try:
            solaros.expansion.detach(target)
            break
        except OSError:
            if attempt == 249:
                raise
            solaros.time.sleep_ms(20)
solaros.streams.tcp.remove(name)
print("DLP_NAMED_PY_PASS")
'''.replace('ADDRESS', json.dumps(address)).replace('PORT', str(port))
    named_lua = '''local solaros = require("solaros")
local name, target = "dlp-lua-hil", "lua-dlp-hil"
solaros.streams.tcp.connect(name, ADDRESS, PORT)
solaros.expansion.attach("stream-dlp", target, {stream = name, ["timeout-ms"] = 3000})
local ok, err = pcall(function()
    solaros.gfx.begin(target)
    assert(not pcall(function() solaros.expansion.detach(target) end))
    solaros.gfx.clear(solaros.gfx.WHITE)
    solaros.gfx.present()
end)
solaros.gfx["end"]()
local detached, detach_err
for attempt = 1, 250 do
    detached, detach_err = pcall(function() solaros.expansion.detach(target) end)
    if detached then break end
    solaros.time.sleep_ms(20)
end
assert(detached, detach_err)
solaros.streams.tcp.remove(name)
if not ok then error(err) end
print("DLP_NAMED_LUA_PASS")
'''.replace('ADDRESS', json.dumps(address)).replace('PORT', str(port))
    return {'dlp-host-hil.py': python, 'dlp-host-hil.lua': lua,
        'dlp-host-named.py': named_python, 'dlp-host-named.lua': named_lua,
        'dlp-host-error.py': 'import solaros\nlink = solaros.dlp.connect("dlp-py-hil")\nraise RuntimeError("DLP_EXPECTED_ERROR")\n',
        'dlp-host-error.lua': 'local link = solaros.dlp.connect("dlp-lua-hil")\nerror("DLP_EXPECTED_ERROR")\n'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--host-address', required=True)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--port', type=int, default=17777)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--private-only', action='store_true', help='Skip named-display checks')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    console = Console(args.host)
    bridge = None
    started_ftp = False
    uploaded = False
    attached = False
    owned_streams = set()
    log = []

    def command(text, seconds=20):
        out = console.command(text, seconds)
        log.append(text + '\n' + out)
        print(text + '\n' + out, flush=True)
        return out

    def ftp(action):
        nonlocal started_ftp
        assert 'job started: ftpd' in command('job start ftpd /sdcard 2121')
        started_ftp = True
        try:
            with ftplib.FTP() as client:
                client.connect(args.host, 2121, timeout=15)
                client.login()
                action(client)
        finally:
            command('job stop ftpd')
            started_ftp = False

    files = probes(args.host_address, args.port)
    try:
        assert not re.search(r'\brunning\b', command('job status ftpd')), 'FTP is already running'
        existing = command('stream list')
        for name in ('dlp-hil', 'dlp-py-hil', 'dlp-lua-hil'):
            assert name not in existing, 'HIL stream already exists'
        assert 'dlp-hil-screen' not in command('expansion devices'), 'HIL display already exists'
        bridge = Bridge(args.port, args.serial)
        if not args.private_only:
            assert 'configured' in command(f'stream tcp connect dlp-hil {args.host_address} {args.port}')
            owned_streams.add('dlp-hil')
            # Complete stream= using the real foreground shell. A unique prefix is used.
            console.socket.sendall(b'expansion attach stream-dlp dlp-hil-screen stream=dlp-h\t')
            completed = console.read(1)
            assert 'dlp-hil' in completed, completed
            console.socket.sendall(b'\r\n')
            out = console.read(20, True)
            log.append('completed stream= attachment\n' + completed + out)
            print(out, flush=True)
            assert 'attached' in out, out
            attached = True
            assert 'dlp-hil-screen' in command('display list')
            assert 'drew' in command('display test dlp-hil-screen')
            assert 'Release the stream owner' in command('stream tcp remove dlp-hil')
            assert 'detached' in command('expansion detach dlp-hil-screen')
            attached = False
            assert 'removed' in command('stream tcp remove dlp-hil')
            owned_streams.remove('dlp-hil')
        command('display list')
        def upload(client):
            for filename, content in files.items():
                client.storbinary('STOR /' + filename, io.BytesIO(content.encode()))
        ftp(upload)
        uploaded = True
        for language, filename, name, marker in (
                ('python', 'dlp-host-hil.py', 'dlp-py-hil', 'DLP_PY_PASS'),
                ('lua', 'dlp-host-hil.lua', 'dlp-lua-hil', 'DLP_LUA_PASS')):
            owned_streams.add(name)
            if not args.private_only:
                extension = 'py' if language == 'python' else 'lua'
                out = command(f'{language} /sdcard/dlp-host-named.{extension}', 30)
                assert 'DLP_NAMED_' + ('PY' if language == 'python' else 'LUA') + '_PASS' in out, out
            assert marker in command(f'{language} /sdcard/{filename}', 40)
            assert 'DLP_EXPECTED_ERROR' in command(f'{language} /sdcard/dlp-host-error.' + ('py' if language == 'python' else 'lua'), 20)
            assert 'removed' in command('stream tcp remove ' + name), 'interpreter leaked stream claim'
            owned_streams.remove(name)
        assert 'dlp-hil-screen' not in command('display list')
        assert not bridge.errors, bridge.errors
        print(f'DLP_HOST_PASS: {bridge.count} TCP sessions, TX={bridge.tx} RX={bridge.rx}', flush=True)
    finally:
        if attached:
            command('expansion detach dlp-hil-screen')
        for target, stream in (('py-dlp-hil', 'dlp-py-hil'), ('lua-dlp-hil', 'dlp-lua-hil')):
            if stream in owned_streams:
                command('expansion detach ' + target)
        for name in owned_streams:
            command('stream tcp remove ' + name)
        if started_ftp:
            command('job stop ftpd')
        if uploaded:
            def remove(client):
                for filename in files:
                    client.delete('/' + filename)
            ftp(remove)
        console.socket.close()
        if bridge:
            bridge.close()
        (args.output / 'console.log').write_text('\n'.join(log))


if __name__ == '__main__':
    main()
