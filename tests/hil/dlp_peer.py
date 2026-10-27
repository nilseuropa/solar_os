#!/usr/bin/env python3
"""Validate DLP discovery retry and foreground streaming between two SolarOS devices.

The receiver must have an unclaimed TCP listener and no active DLP app. The
sender must have Game Boy installed and a user-supplied ROM. Temporary streams,
displays and sessions are removed. The receiver's existing listener is retained.
"""
import argparse
from pathlib import Path
import re
import select
import socket
import struct
import threading
import time

from model_bundle_stream import Console


class DropFirstConnection:
    def __init__(self, port, destination):
        self.destination = destination
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(('0.0.0.0', port))
        self.listener.listen(2)
        self.listener.settimeout(.1)
        self.stopping = threading.Event()
        self.connections = 0
        self.errors = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            while not self.stopping.is_set():
                try:
                    client, _ = self.listener.accept()
                except socket.timeout:
                    continue
                self.connections += 1
                with client:
                    if self.connections == 1:
                        client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                        continue  # Simulate a listener fencing its previous peer.
                    with socket.create_connection(self.destination, 3) as receiver:
                        for peer in (client, receiver):
                            peer.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                            peer.settimeout(2)
                        active = True
                        while active and not self.stopping.is_set():
                            ready, _, _ = select.select([client, receiver], [], [], .05)
                            for peer in ready:
                                data = peer.recv(8192)
                                if not data:
                                    active = False
                                    break
                                (receiver if peer is client else client).sendall(data)
        except Exception as error:
            if not self.stopping.is_set():
                self.errors.append(repr(error))

    def close(self):
        self.stopping.set()
        self.thread.join(4)
        self.listener.close()
        assert not self.thread.is_alive(), 'proxy did not stop'
        assert not self.errors, self.errors


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sender', required=True)
    p.add_argument('--receiver', required=True)
    p.add_argument('--host-address', required=True)
    p.add_argument('--receiver-stream', default='tcp0')
    p.add_argument('--receiver-port', type=int, default=7777)
    p.add_argument('--proxy-port', type=int, default=18110)
    p.add_argument('--rom', required=True)
    p.add_argument('--seconds', type=int, default=40)
    p.add_argument('--output', required=True, type=Path)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    sender, receiver = Console(a.sender), Console(a.receiver)
    proxy = None
    serving = attached = False
    streams, sessions, transcript, receive_counts = set(), [], [], []

    def command(console, text, timeout=25):
        out = console.command(text, timeout)
        label = 'sender' if console is sender else 'receiver'
        transcript.append(label + ': ' + text + '\n' + out)
        print(label, text + '\n' + out, flush=True)
        return out

    def frames():
        out = command(receiver, 'dlp status')
        match = re.search(r'frames=(\d+)', out)
        assert match, out
        receive_counts.append(int(re.search(r'RX=(\d+)', out)[1]))
        return int(match[1])

    try:
        command(sender, 'board')
        command(receiver, 'board')
        assert 'DLP: stopped' in command(receiver, 'dlp status')
        existing = command(sender, 'stream list')
        assert not any(name in existing for name in ('dlp-retry', 'dlp-peer'))
        assert 'dlp-test' not in command(sender, 'display list')
        assert 'DLP session' in command(receiver, f'dlp serve display0 stream={a.receiver_stream}')
        serving = True
        proxy = DropFirstConnection(a.proxy_port, (a.receiver, a.receiver_port))
        assert 'configured' in command(sender, f'stream tcp connect dlp-retry {a.host_address} {a.proxy_port}')
        streams.add('dlp-retry')
        assert 'attached dlp-test' in command(sender, 'expansion attach stream-dlp dlp-test stream=dlp-retry')
        attached = True
        assert proxy.connections >= 2, 'initial TCP reset was not retried'
        assert 'drew' in command(sender, 'display test dlp-test')
        assert 'detached' in command(sender, 'expansion detach dlp-test')
        attached = False
        assert 'removed' in command(sender, 'stream tcp remove dlp-retry')
        streams.remove('dlp-retry')
        proxy.close()
        proxy = None
        assert 'configured' in command(sender, f'stream tcp connect dlp-peer {a.receiver} {a.receiver_port}')
        streams.add('dlp-peer')
        assert 'attached dlp-test' in command(sender, 'expansion attach stream-dlp dlp-test stream=dlp-peer')
        attached = True
        first = frames()
        first_rx = receive_counts[-1]
        out = command(sender, f'session create gameboy dlp-test {a.rom}')
        match = re.search(r'session (\d+) created', out)
        assert match, out
        gameboy = match[1]
        sessions.append(gameboy)
        deadline = time.monotonic() + a.seconds
        minimum_stack = None
        while time.monotonic() < deadline:
            time.sleep(5)
            out = command(sender, 'top')
            match = re.search(r'^frame_presen\S*\s+\S+\s+\d+\s+[\d.]+\s+([\d.]+) (KiB|B)', out, re.M)
            assert match, 'frame presenter disappeared: ' + out
            free = float(match[1]) * (1024 if match[2] == 'KiB' else 1)
            minimum_stack = free if minimum_stack is None else min(minimum_stack, free)
            assert free >= 1024, 'insufficient stack margin: ' + str(free)
            frames()
        # A supplied ROM may stop on a static title screen. Unchanged frames
        # use retained-state queries rather than repeat panel presentations.
        assert frames() > first, 'receiver did not present a game frame'
        assert receive_counts[-1] > first_rx + 1000, 'presenter stopped communicating'
        out = command(sender, 'session create clock dlp-test')
        match = re.search(r'session (\d+) created', out)
        assert match, out
        clock = match[1]
        sessions.append(clock)
        time.sleep(1)
        assert 'closed session' in command(sender, 'session close ' + clock)
        sessions.remove(clock)
        before = frames()
        time.sleep(5)
        assert frames() > before, 'Game Boy did not resume'
        # An auxiliary display can render while its session is detached from
        # keyboard focus on the sender's primary display.
        assert re.search(r'gameboy\s+gameboy\s+(?:active|detached)\s+dlp-test', command(sender, 'sessions'))
        command(sender, 'log show 10')
        print(f'DLP_PEER_PASS: discovery recovered from reset; minimum presenter stack {minimum_stack} bytes', flush=True)
    finally:
        if attached:
            # Creating an app on an auxiliary display also creates a parent
            # shell. Close every session on this test-owned target, including
            # that shell, before detaching it.
            live = command(sender, 'sessions')
            owned = re.findall(r'^\s*(\d+)\s+.*\sdlp-test\s', live, re.M)
            for session in sorted(owned, key=int, reverse=True):
                command(sender, 'session close ' + session)
            command(sender, 'expansion detach dlp-test')
        for stream in streams:
            command(sender, 'stream tcp remove ' + stream)
        if serving:
            command(receiver, 'dlp stop display0')
        sender.socket.close()
        receiver.socket.close()
        if proxy:
            proxy.close()
        (a.output / 'console.log').write_text('\n'.join(transcript))


if __name__ == '__main__':
    main()
