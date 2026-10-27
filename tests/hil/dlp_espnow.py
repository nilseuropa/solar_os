#!/usr/bin/env python3
"""Validate two-device DLP over ESP-NOW, reconnects and named-display cleanup.

Both devices need an idle espnow-link job. The sender needs Python and SD.
Only temporary test-owned jobs, streams, displays and files are removed.
"""
import argparse
import ftplib
import io
from pathlib import Path
import re
import threading
import time

from model_bundle_stream import Console


PROBE = '''import solaros, sys
mode = sys.argv[1]
if mode == "terminal":
    handle = solaros.streams.open("dlp-hil", {"direction": "duplex"})
    try:
        def prompt():
            data = b""
            start = solaros.time.uptime_ms()
            while solaros.time.uptime_ms() - start < 20000:
                try:
                    data += solaros.streams.read(handle, 1024, 1000)
                except OSError:
                    continue
                if data.endswith(b":/ ") and b"@" in data:
                    return data
            raise RuntimeError("Remote shell prompt timed out")
        banner = prompt()
        times = []
        for i in range(10):
            start = solaros.time.uptime_ms()
            written = solaros.streams.write(handle, b"version\\r", 5000)
            assert written == 8
            assert b"SolarOS" in prompt()
            times.append(solaros.time.uptime_ms() - start)
        print("LINK_TERMINAL_RTT_MS", min(times), sum(times) / len(times), max(times))
        start = solaros.time.uptime_ms()
        written = solaros.streams.write(handle, b"commands\\r", 5000)
        assert written == 9
        reply = prompt()
        assert len(reply) > 512
        elapsed = solaros.time.uptime_ms() - start
        print("LINK_TERMINAL_BURST", len(reply), "bytes", elapsed, "ms")
        print("LINK_TERMINAL_PASS")
    finally:
        solaros.streams.close(handle)
    sys.exit()
with solaros.dlp.connect("dlp-hil", timeout_ms=5000) as link:
    info = link.info()
    assert info["format"] == solaros.dlp.MONO1
    size, stride = info["frame_bytes"], info["stride"]
    white = bytes(size)
    patch = bytearray(size)
    for y in range(info["height"] // 2, info["height"] // 2 + 32):
        for x in range(4):
            patch[y * stride + x] = 255
    frames = (white, bytes(patch))
    if mode == "benchmark":
        black = bytes([255]) * size
        dense = bytes([0x55, 0xaa]) * (size // 2)
        inverse = bytes([0xaa, 0x55]) * (size // 2)
        cases = (("solid", (white, black), 8),
                 ("full-detail", (dense, inverse), 4),
                 ("32x32-patch", frames, 8))
        for name, images, samples in cases:
            link.present(images[1])
            start = solaros.time.uptime_ms()
            times = []
            for i in range(samples):
                before = solaros.time.uptime_ms()
                link.present(images[i % 2])
                times.append(solaros.time.uptime_ms() - before)
            elapsed = solaros.time.uptime_ms() - start
            print("DLP_BENCH", name, "ms", min(times), sum(times) / samples,
                  max(times), "fps", samples * 1000 / elapsed)
    count = 0 if mode == "benchmark" else (40 if mode == "continuous" else 2)
    for i in range(count):
        for attempt in range(12):
            try:
                link.present(frames[i % 2])
                break
            except OSError as error:
                print("DLP_HIL_RETRY", i, attempt, error)
                if attempt == 11:
                    raise
                solaros.time.sleep_ms(100)
        print("DLP_HIL_FRAME", i)
        if mode == "continuous":
            solaros.time.sleep_ms(150)
    print("DLP_HIL_PASS", count)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sender', required=True)
    parser.add_argument('--receiver', required=True)
    parser.add_argument('--cycles', type=int, default=5)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--benchmark-only', action='store_true',
                      help='measure completed presentations and skip lifecycle tests')
    mode.add_argument('--terminal-only', action='store_true',
                      help='measure sparse commands and a command-list burst without DLP')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert 2 <= args.cycles <= 20
    args.output.mkdir(parents=True, exist_ok=True)
    sender, receiver = Console(args.sender), Console(args.receiver)
    transcript = []
    jobs, streams = [], []
    attached = serving = uploaded = ftp_started = False
    terminal_session = None
    terminal_running = False
    filename = '/dlp-espnow-hil.py'
    worker = None
    log_lock = threading.Lock()

    def command(console, text, seconds=25):
        out = console.command(text, seconds)
        label = 'sender' if console is sender else 'receiver'
        line = label + ': ' + text + '\n' + out
        with log_lock:
            transcript.append(line)
            (args.output / 'console.log').write_text('\n'.join(transcript))
            print(line, flush=True)
        return out

    def frames():
        out = command(receiver, 'dlp status')
        assert re.search(r'DLP: (?:serving|waiting)', out), out
        return int(re.search(r'frames=(\d+)', out)[1])

    def memory():
        # Let the preceding console output leave lwIP's send queues before
        # sampling the SDK heap; those network buffers also live in PSRAM.
        time.sleep(.25)
        out = command(sender, 'mem policy')
        match = re.search(r'Managed PSRAM: total (\d+) free (\d+).*?, (\d+) allocations', out)
        assert match, 'Managed arena is required for precise allocation checks'
        sdk = re.search(r'SDK PSRAM: free (\d+)', out)
        assert sdk, out
        return int(match[2]), int(match[3]), int(sdk[1])

    def run_probe():
        before = frames()
        out = command(sender, 'python ' + filename + ' probe', 60)
        assert 'DLP_HIL_PASS 2' in out, out
        assert frames() >= before + 2, 'No two completed presentations'

    def run_terminal():
        nonlocal terminal_session, terminal_running
        out = command(receiver, 'session create shell dlp-hil --term dumb')
        match = re.search(r'session (\d+) created', out)
        assert match, out
        terminal_session = match[1]
        terminal_running = True
        out = command(sender, 'python ' + filename + ' terminal', 120)
        assert 'LINK_TERMINAL_PASS' in out, out
        terminal_running = False
        assert 'closed session' in command(receiver, 'session close ' + terminal_session)
        terminal_session = None

    def check_released(baseline):
        after = memory()
        assert after[:2] == baseline[:2], f'Managed allocation leak: {baseline} -> {after}'
        # The SDK region includes live Telnet/lwIP buffers. Keep the allowance
        # below any DLP connection or image allocation, across all cycles.
        assert after[2] >= baseline[2] - 128, f'SDK heap did not recover: {baseline} -> {after}'
        return after

    def restart_during_frames(which):
        nonlocal worker, serving
        before = frames()
        result = {}
        def run():
            try:
                result['out'] = command(sender, 'python ' + filename + ' continuous', 180)
            except Exception as error:
                result['error'] = error
        worker = threading.Thread(target=run)
        worker.start()
        deadline = time.monotonic() + 25
        while frames() < before + 3:
            assert worker.is_alive() and time.monotonic() < deadline
            time.sleep(.2)
        if which == 'provider':
            assert 'closed session' in command(receiver, 'dlp stop display0')
            serving = False
        else:
            assert 'job stopped' in command(receiver, 'job stop espnow-link')
        time.sleep(2)
        resume_base = 0 if which == 'provider' else frames()
        resumed_at = time.monotonic()
        if which == 'provider':
            assert 'DLP session' in command(receiver, 'dlp serve display0 stream=dlp-hil')
            serving = True
        else:
            assert 'job started' in command(receiver,
                'job start espnow-link dlp-hil channel=auto phy=normal')
        while frames() <= resume_base:
            assert time.monotonic() - resumed_at < 25, 'No presentation after restart'
            assert worker.is_alive(), 'Producer ended before presentation resumed'
            time.sleep(.2)
        print('DLP_RECONNECT_PASS', which, 'first_frame_ms',
              round((time.monotonic() - resumed_at) * 1000), flush=True)
        worker.join(1)
        deadline = time.monotonic() + 120
        while worker.is_alive() and time.monotonic() < deadline:
            worker.join(5)
            command(receiver, 'link stream status dlp-hil')
        assert not worker.is_alive(), 'Producer did not finish after reconnect'
        assert 'error' not in result, result
        assert 'DLP_HIL_PASS 40' in result['out'], result
        assert frames() >= (3 if which == 'provider' else before + 40)
        worker = None
        run_probe()  # A new consumer must also open successfully afterwards.

    try:
        channels = []
        for console in (sender, receiver):
            command(console, 'board')
            channels.append(re.search(r'Link: ch (\d+)', command(console, 'wifi status'))[1])
            assert re.search(r'espnow-link\s+stopped', command(console, 'job status espnow-link'))
            assert 'dlp-hil' not in command(console, 'stream list')
        assert channels[0] == channels[1], 'Wi-Fi channels must match for coexistence'
        assert 'DLP: stopped' in command(receiver, 'dlp status')
        assert 'dlp-hil' not in command(sender, 'display list')
        assert re.search(r'ftpd\s+stopped', command(sender, 'job status ftpd'))
        assert 'No such' in command(sender, 'ls ' + filename)
        ids = []
        for console in (sender, receiver):
            assert 'job started' in command(console,
                'job start espnow-link dlp-hil channel=auto phy=normal')
            jobs.append(console)
            ids.append(re.search(r'id=(0x[0-9a-fA-F]+)', command(console, 'link status dlp-hil'))[1])
        for _ in range(2):
            for console in (sender, receiver):
                command(console, 'link send dlp-hil broadcast DLP-HIL')
        for console, peer in ((sender, ids[1]), (receiver, ids[0])):
            assert peer.lower() in command(console, 'espnow peers').lower()
            assert 'registered' in command(console, 'link stream create dlp-hil dlp-hil ' + peer)
            streams.append(console)
        if not args.terminal_only:
            assert 'DLP session' in command(receiver, 'dlp serve display0 stream=dlp-hil')
            serving = True
        assert 'job started' in command(sender, 'job start ftpd /sdcard 2121')
        ftp_started = True
        with ftplib.FTP() as ftp:
            ftp.connect(args.sender, 2121, timeout=15)
            ftp.login()
            uploaded = True  # Also clean up a partially uploaded file.
            ftp.storbinary('STOR ' + filename, io.BytesIO(PROBE.encode()))
        command(sender, 'job stop ftpd')
        ftp_started = False
        if args.terminal_only:
            run_terminal()
            for console in (sender, receiver):
                command(console, 'link status dlp-hil')
                command(console, 'link stream status dlp-hil')
            print('LINK_ESPNOW_TERMINAL_PASS', flush=True)
            return
        run_probe()
        before = frames()
        out = command(sender, 'python ' + filename + ' benchmark', 180)
        assert 'DLP_HIL_PASS 0' in out and out.count('DLP_BENCH ') == 3, out
        assert frames() >= before + 23, 'Benchmark frames did not complete'
        if args.benchmark_only:
            print('DLP_ESPNOW_BENCHMARK_PASS', flush=True)
            return
        baseline = memory()
        attached_bytes = []
        for cycle in range(args.cycles):
            assert 'attached dlp-hil' in command(sender,
                'expansion attach stream-dlp dlp-hil stream=dlp-hil')
            attached = True
            live = memory()
            assert live[0] < baseline[0] - 300000 and live[1] > baseline[1]
            attached_bytes.append(baseline[0] - live[0])
            before = frames()
            assert 'drew' in command(sender, 'display test dlp-hil', 45)
            assert frames() == before + 1, 'Test image did not actually reach the panel'
            if cycle == args.cycles - 1:
                out = command(sender, 'session create clock dlp-hil')
                assert re.search(r'session \d+ created', out), out
                assert 'is busy' in command(sender, 'expansion detach dlp-hil')
                live_sessions = command(sender, 'sessions')
                owned = re.findall(r'^\s*(\d+)\s+.*\sdlp-hil\s', live_sessions, re.M)
                assert len(owned) >= 2, 'Clock and its parent shell must own the target'
                for session in sorted(owned, key=int, reverse=True):
                    assert 'closed session' in command(sender, 'session close ' + session, 45)
            assert 'detached' in command(sender, 'expansion detach dlp-hil')
            attached = False
            check_released(baseline)
            command(sender, 'link stream status dlp-hil')
        print('DLP_DETACH_PASS', args.cycles, 'cycles; released bytes:', attached_bytes, flush=True)
        for which in ('transport', 'provider', 'transport', 'provider'):
            restart_during_frames(which)
        # Private objects and reconnects must release their allocations too.
        check_released(baseline)
        assert 'closed session' in command(receiver, 'dlp stop display0')
        serving = False
        run_terminal()
        for console in (sender, receiver):
            command(console, 'link status dlp-hil')
            command(console, 'link stream status dlp-hil')
            command(console, 'espnow status')
            command(console, 'log show 10')
        print('DLP_ESPNOW_PASS: presentation, repeated detach, transport and provider reconnect', flush=True)
    finally:
        if terminal_running:
            sender.socket.sendall(b'\x03')
            sender.read(5, True)
        if worker and worker.is_alive():
            sender.socket.sendall(b'\x03')
            worker.join(15)
        if attached:
            live_sessions = command(sender, 'sessions')
            for session in sorted(re.findall(r'^\s*(\d+)\s+.*\sdlp-hil\s', live_sessions, re.M),
                                  key=int, reverse=True):
                command(sender, 'session close ' + session, 45)
            command(sender, 'expansion detach dlp-hil')
        if serving:
            command(receiver, 'dlp stop display0')
        if terminal_session:
            command(receiver, 'session close ' + terminal_session)
        if ftp_started:
            command(sender, 'job stop ftpd')
        for console in streams:
            command(console, 'link stream remove dlp-hil')
        for console in jobs:
            command(console, 'job stop espnow-link')
        if uploaded:
            command(sender, 'rm -f ' + filename)
        for console in (sender, receiver):
            command(console, 'sessions')
            command(console, 'dlp status')
            console.socket.close()


if __name__ == '__main__':
    main()
