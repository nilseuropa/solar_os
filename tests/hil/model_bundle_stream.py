#!/usr/bin/env python3
"""Validate an installed bundle's RTSP loop against an owned local test feed."""
import argparse
import json
from pathlib import Path
import re
import select
import socket
import subprocess
import time


class Console:
    def __init__(self, host):
        self.socket = socket.create_connection((host, 23), 5)
        self.read(1)

    def read(self, seconds, prompt=False):
        output = bytearray()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.socket], [], [], .1)
            if not ready:
                continue
            data = self.socket.recv(65536)
            if not data:
                raise RuntimeError("console ended")
            output.extend(data)
            if prompt and re.search(rb'\w+@[^\r\n]+:/[^\r\n]* ', output):
                break
        return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', output.decode(errors='replace'))

    def command(self, command, seconds=15):
        self.socket.sendall((command + '\r\n').encode())
        return self.read(seconds, True)


def records(output):
    result = []
    for line in output.splitlines():
        if not line.startswith('{'):
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue  # Cancellation can interrupt console printing.
        if value.get('event') == 'inference':
            result.append(value)
    return result


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--telnet', required=True)
    parser.add_argument('--host-address', required=True)
    parser.add_argument('--mediamtx', required=True)
    parser.add_argument('--bundle', required=True, help='Installed device bundle.json')
    parser.add_argument('--runner', default='/dl/infer.py')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--count', type=int, default=12)
    parser.add_argument('--port', type=int, default=18654)
    parser.add_argument('--rtp-port', type=int, default=18100)
    parser.add_argument('--interrupt', action='store_true')
    args = parser.parse_args()
    if not 3 <= args.count <= 100 or not 1024 <= args.port <= 65535 or not 1024 <= args.rtp_port < 65535:
        parser.error('invalid count/ports')
    args.output.mkdir(parents=True, exist_ok=False)
    for port, kind in ((args.port,socket.SOCK_STREAM),(args.rtp_port,socket.SOCK_DGRAM),(args.rtp_port+1,socket.SOCK_DGRAM)):
        with socket.socket(socket.AF_INET,kind) as probe:
            probe.bind(('0.0.0.0',port))
    config = args.output/'server.yml'
    config.write_text('readTimeout: 30s\nrtsp: true\nrtspAddress: :%d\nrtspTransports: [udp]\n'
        'rtpAddress: :%d\nrtcpAddress: :%d\nrtmp: false\nhls: false\nwebrtc: false\nsrt: false\nmoq: false\n'
        'paths:\n  all_others:\n    source: publisher\n' % (args.port,args.rtp_port,args.rtp_port+1))
    server = publisher = console = None
    running = False
    handle = None
    with (args.output/'server.log').open('w') as server_log, (args.output/'publisher.log').open('w') as publisher_log:
        try:
            server = subprocess.Popen([args.mediamtx,str(config)],stdout=server_log,stderr=subprocess.STDOUT)
            time.sleep(1)
            if server.poll() is not None:
                raise RuntimeError('MediaMTX startup failed')
            publisher = subprocess.Popen(['ffmpeg','-hide_banner','-loglevel','warning','-re','-f','lavfi',
                '-i','testsrc2=size=160x120:rate=10','-c:v','mjpeg','-threads','1','-pix_fmt','yuvj420p',
                '-q:v','6','-huffman','default','-force_duplicated_matrix','1','-an','-f','rtsp',
                '-rtsp_transport','udp','rtsp://127.0.0.1:%d/bundle-test'%args.port],
                stdout=publisher_log,stderr=subprocess.STDOUT)
            time.sleep(2)
            if publisher.poll() is not None:
                raise RuntimeError('publisher startup failed')
            console = Console(args.telnet)
            url = 'rtsp://%s:%d/bundle-test'%(args.host_address,args.port)
            loaded = console.command('model bundle '+args.bundle,90)
            (args.output/'load.log').write_text(loaded)
            match = re.search(r'Resident model (\d+)',loaded)
            if not match:
                raise RuntimeError('bundle load failed; inspect load.log')
            handle = int(match.group(1))
            command = 'python %s %d %s'%(args.runner,handle,url)
            (args.output/'before.log').write_text(console.command('mem')+console.command('top'))
            running = True
            output = console.command(command+' --count '+str(args.count),120)
            (args.output/'finite.log').write_text(output)
            found = records(output)
            if len(found) != args.count:
                raise RuntimeError('missing results or runtime error; inspect finite.log')
            running = False
            for index, value in enumerate(found):
                assert value['sequence']==index
                assert value['model_handle']==handle
                assert value['frame']['network']
                assert value['transforms'][next(iter(value['transforms']))]['source_width']==160
                assert value['transforms'][next(iter(value['transforms']))]['source_height']==120
            stamps = [value['frame']['timestamp_us'] for value in found]
            assert all(a < b for a,b in zip(stamps,stamps[1:])), 'stale/reused frame timestamp'
            if args.interrupt:
                console.socket.sendall((command+'\r\n').encode())
                running = True
                output = console.read(12)
                console.socket.sendall(b'\x1d')
                output += console.read(30,True)
                (args.output/'interrupt.log').write_text(output)
                assert len(records(output)) >= 3, 'interruption did not exercise active inference'
                assert re.search(r'\w+@[^\r\n]+:/[^\r\n]* ',output), 'no shell prompt after cancellation'
                running = False
            info = console.command('model info '+str(handle))
            assert 'handle=%d '%handle in info, 'model disappeared after script exit'
            after = info+console.command('mem')+console.command('top')
            (args.output/'after.log').write_text(after)
            assert not re.search(r'^(?:inference|rtsp_client|rtsp_rx|dl_mc[01])\s',after,re.M)
            (args.output/'records.json').write_text(json.dumps(found,indent=2)+'\n')
            released = console.command('model unload '+str(handle))
            assert 'model:' not in released, 'explicit unload failed'
            stale = console.command('model info '+str(handle))
            assert 'not found' in stale, 'unloaded handle remains valid'
            handle = None
            (args.output/'released.log').write_text(released+stale+console.command('mem')+console.command('top'))
            print('MODEL_BUNDLE_STREAM_OK',len(found),'results; residency, explicit unload and cancellation passed' if args.interrupt else 'results; residency and explicit unload passed')
        finally:
            if console is not None:
                if running:
                    try:
                        console.socket.sendall(b'\x1d')
                        console.read(15,True)
                    except OSError:
                        pass
                if handle is not None:
                    try:
                        console.command('model unload '+str(handle),30)
                    except (OSError,RuntimeError):
                        pass
                console.socket.close()
            stop(publisher)
            stop(server)


if __name__=='__main__':
    main()
