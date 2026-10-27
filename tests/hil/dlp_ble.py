#!/usr/bin/env python3
"""Two-device BLE CoC/DLP discovery, reconnect, detach and keyboard coexistence.

Both consoles must be idle. The central needs Python, FTP and writable SD.
Only test-owned endpoints, displays, sessions and files are removed.
"""
import argparse
import ftplib
import io
from pathlib import Path
import re
import sys
import threading
import time

from model_bundle_stream import Console


PROBE = '''import solaros, sys
mode = sys.argv[1]
solaros.streams.ble.listen("ble-bind-hil")
solaros.streams.ble.remove("ble-bind-hil")
solaros.streams.ble.connect("ble-bind-hil", "aa:bb:cc:dd:ee:ff", 1)
solaros.streams.ble.remove("ble-bind-hil")
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
            raise RuntimeError("BLE shell prompt timed out")
        banner = prompt()
        times = []
        for i in range(10):
            start = solaros.time.uptime_ms()
            written = solaros.streams.write(handle, b"version\\r", 5000)
            assert written == 8
            reply = prompt()
            assert b"SolarOS" in reply
            times.append(solaros.time.uptime_ms() - start)
        print("BLE_TERMINAL_RTT_MS", min(times), sum(times) / len(times), max(times))
        written = solaros.streams.write(handle, b"commands\\r", 5000)
        reply = prompt()
        assert written == 9 and len(reply) > 512
        print("BLE_TERMINAL_PASS", len(reply), "bytes")
    finally:
        solaros.streams.close(handle)
    sys.exit()
with solaros.dlp.connect("dlp-hil", timeout_ms=8000) as link:
    info = link.info()
    assert info["format"] == solaros.dlp.MONO1
    size, stride = info["frame_bytes"], info["stride"]
    white = bytes(size)
    patch = bytearray(size)
    for y in range(info["height"] // 2, info["height"] // 2 + 32):
        for x in range(4):
            patch[y * stride + x] = 255
    images = (white, bytes(patch))
    if mode == "benchmark":
        cases = (("solid", (white, bytes([255]) * size), 8),
                 ("full-detail", (bytes([0x55, 0xaa]) * (size // 2),
                                  bytes([0xaa, 0x55]) * (size // 2)), 4),
                 ("32x32-patch", images, 8))
        for name, frames, count in cases:
            link.present(frames[1])
            start = solaros.time.uptime_ms()
            for i in range(count):
                link.present(frames[i % 2])
                print("BLE_DLP_BENCH_FRAME", name, i,
                      solaros.time.uptime_ms() - start)
            elapsed = solaros.time.uptime_ms() - start
            print("BLE_DLP_BENCH", name, "fps", count * 1000 / elapsed)
    count = 30 if mode == "continuous" else (0 if mode == "benchmark" else 2)
    for i in range(count):
        for attempt in range(12):
            try:
                link.present(images[i % 2])
                break
            except OSError as error:
                print("BLE_DLP_RETRY", i, attempt, error)
                if attempt == 11:
                    raise
                solaros.time.sleep_ms(100)
        print("BLE_DLP_FRAME", i)
        if mode == "continuous":
            solaros.time.sleep_ms(200)
    print("BLE_DLP_PROBE_PASS", count)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--central', required=True)
    parser.add_argument('--peripheral', required=True)
    parser.add_argument('--cycles', type=int, default=3)
    parser.add_argument('--require-keyboards', action='store_true',
                        help='require both keyboard links before and throughout the test')
    parser.add_argument('--skip-benchmark', action='store_true',
                        help='run functional checks without repeating the throughput benchmark')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert 2 <= args.cycles <= 20
    args.output.mkdir(parents=True, exist_ok=True)
    central, peripheral = Console(args.central), Console(args.peripheral)
    # Port-shell startup can finish after Console's short initial greeting
    # read. Drain both welcomes before pairing commands with their replies.
    central.read(2)
    peripheral.read(2)
    transcript = []
    log_lock = threading.Lock()
    configured = []
    serving = attached = uploaded = ftp_started = False
    filename = '/dlp-ble-hil.py'
    worker = None
    worker_console = central
    terminal_session = None
    terminal_running = False
    reverse_attached = reverse_serving = False

    def command(console, text, seconds=25):
        out = console.command(text, seconds)
        line = ('central' if console is central else 'peripheral') + ': ' + text + '\n' + out
        with log_lock:
            transcript.append(line)
            (args.output / 'console.log').write_text('\n'.join(transcript))
            print(line, flush=True)
        return out

    def frames():
        out = command(peripheral, 'dlp status')
        assert re.search(r'DLP: (?:serving|waiting)', out), out
        return int(re.search(r'frames=(\d+)', out)[1])

    def memory():
        time.sleep(.3)
        out = command(central, 'mem policy')
        m = re.search(r'Managed PSRAM: total (\d+) free (\d+).*?, (\d+) allocations', out)
        sdk = re.search(r'SDK PSRAM: free (\d+)', out)
        assert m and sdk, out
        return int(m[2]), int(m[3]), int(sdk[1])

    def probe(mode='probe'):
        nonlocal worker, worker_console
        before = frames()
        result = {}
        def run():
            try:
                result['out'] = command(central, 'python ' + filename + ' ' + mode, 240)
            except Exception as error:
                result['error'] = error
        task = threading.Thread(target=run)
        worker, worker_console = task, central
        task.start()
        while task.is_alive():
            task.join(3)
            if task.is_alive():
                command(peripheral, 'stream ble status dlp-hil')
                frames()
        assert 'error' not in result, result
        worker = None
        out = result['out']
        if 'BLE_DLP_PROBE_PASS' not in out:
            command(central, 'stream ble status dlp-hil')
            command(peripheral, 'stream ble status dlp-hil')
            frames()
        expected = 30 if mode == 'continuous' else 0 if mode == 'benchmark' else 2
        assert 'BLE_DLP_PROBE_PASS ' + str(expected) in out, out
        assert frames() >= before + (23 if mode == 'benchmark' else expected)
        for console in (central, peripheral):
            status = command(console, 'ble status')
            if args.require_keyboards:
                assert 'connected keyboard' in status

    try:
        for console in (central, peripheral):
            command(console, 'board')
            status = command(console, 'ble status')
            if args.require_keyboards:
                assert 'connected keyboard' in status
            assert 'dlp-hil' not in command(console, 'stream list')
        assert 'DLP: stopped' in command(peripheral, 'dlp status')
        assert 'dlp-hil' not in command(central, 'display list')
        assert re.search(r'ftpd\s+stopped', command(central, 'job status ftpd'))
        assert 'No such' in command(central, 'ls ' + filename)
        assert 'configured' in command(peripheral, 'stream ble listen dlp-hil')
        configured.append(peripheral)
        assert 'DLP session' in command(peripheral, 'dlp serve display0 stream=dlp-hil')
        serving = True
        found = None
        for attempt in range(3):
            out = command(central, 'ble scan', 35)
            found = re.search(r'(public|random|rpa_public|rpa_random)\s+([0-9a-f:]{17})\s+.*SolarOS dlp-hil', out, re.I)
            if found:
                break
        assert found, 'Stream advertisement not discovered'
        assert 'configured' in command(central, 'stream ble connect dlp-hil ' + found[2] + ' ' + found[1])
        configured.append(central)
        assert 'job started' in command(central, 'job start ftpd /sdcard 2121')
        ftp_started = True
        with ftplib.FTP() as ftp:
            ftp.connect(args.central, 2121, timeout=15)
            ftp.login()
            uploaded = True
            ftp.storbinary('STOR ' + filename, io.BytesIO(PROBE.encode()))
        command(central, 'job stop ftpd')
        ftp_started = False
        probe()
        if not args.skip_benchmark:
            probe('benchmark')
        baseline = memory()
        released = []
        for cycle in range(args.cycles):
            assert 'attached dlp-hil' in command(central, 'expansion attach stream-dlp dlp-hil stream=dlp-hil', 45)
            attached = True
            live = memory()
            released.append(baseline[0] - live[0])
            before = frames()
            command(central, 'display test dlp-hil', 60)
            assert frames() == before + 1
            assert 'connected' in command(central, 'stream ble status dlp-hil')
            assert 'detached' in command(central, 'expansion detach dlp-hil', 45)
            attached = False
            after = memory()
            assert after[:2] == baseline[:2], (baseline, after)
            assert after[2] >= baseline[2] - 512, (baseline, after)
        print('BLE_DLP_DETACH_PASS', released, flush=True)
        for cycle in range(2):
            result = {}
            def continuous():
                try:
                    result['out'] = command(central, 'python ' + filename + ' continuous', 180)
                except Exception as error:
                    result['error'] = error
            before = frames()
            worker = threading.Thread(target=continuous)
            worker.start()
            deadline = time.monotonic() + 30
            while frames() < before + 3:
                assert worker.is_alive() and time.monotonic() < deadline
                time.sleep(.2)
            assert 'closed session' in command(peripheral, 'dlp stop display0')
            serving = False
            time.sleep(2)
            assert 'DLP session' in command(peripheral, 'dlp serve display0 stream=dlp-hil')
            serving = True
            started = time.monotonic()
            while not frames():
                assert worker.is_alive() and time.monotonic() - started < 35
                time.sleep(.2)
            print('BLE_DLP_RECONNECT_PASS', cycle, round((time.monotonic()-started)*1000), 'ms', flush=True)
            worker.join(120)
            assert not worker.is_alive() and 'error' not in result, result
            assert 'BLE_DLP_PROBE_PASS 30' in result['out'], result
            worker = None
            probe()
        after = memory()
        assert after[:2] == baseline[:2] and after[2] >= baseline[2] - 512, (baseline, after)
        assert 'closed session' in command(peripheral, 'dlp stop display0')
        serving = False
        out = command(peripheral, 'session create shell dlp-hil --term dumb')
        match = re.search(r'session (\d+) created', out)
        assert match, out
        terminal_session = match[1]
        terminal_running = True
        out = command(central, 'python ' + filename + ' terminal', 90)
        assert 'BLE_TERMINAL_PASS' in out, out
        terminal_running = False
        # A nonblocking, disconnected port must not starve the idle task or
        # the console that requests shell teardown.
        time.sleep(8)
        assert 'closed session' in command(peripheral, 'session close ' + terminal_session)
        terminal_session = None
        # Retain the BLE roles but reverse the DLP direction. The peripheral
        # becomes a display client; the central serves its local display.
        result = {}
        def reverse_attach():
            try:
                result['out'] = command(peripheral,
                    'expansion attach stream-dlp ble-reverse stream=dlp-hil timeout-ms=8000', 45)
            except Exception as error:
                result['error'] = error
        assert 'ble-reverse' not in command(peripheral, 'display list')
        worker = threading.Thread(target=reverse_attach)
        worker_console = peripheral
        worker.start()
        time.sleep(.3)
        assert 'DLP session' in command(central, 'dlp serve display0 stream=dlp-hil', 30)
        reverse_serving = True
        worker.join(45)
        assert not worker.is_alive() and 'error' not in result, result
        assert 'attached ble-reverse' in result['out'], result
        worker = None
        reverse_attached = True
        command(peripheral, 'display test ble-reverse', 60)
        out = command(central, 'dlp status')
        assert re.search(r'frames=[1-9]', out), out
        assert 'detached' in command(peripheral, 'expansion detach ble-reverse', 45)
        reverse_attached = False
        assert 'closed session' in command(central, 'dlp stop display0', 45)
        reverse_serving = False
        for console in (central, peripheral):
            out = command(console, 'ble status')
            if args.require_keyboards:
                assert 'connected keyboard' in out
        print('BLE_DLP_REVERSE_DIRECTION_PASS', flush=True)
        print('BLE_DLP_PASS', flush=True)
    finally:
        failed = sys.exc_info()[0] is not None
        cleanup_errors = []
        def cleanup(console, text, seconds=45):
            try:
                return command(console, text, seconds)
            except Exception as error:
                cleanup_errors.append((text, str(error)))
                print('BLE_DLP_CLEANUP_ERROR', text, error, flush=True)
        if terminal_running:
            try:
                central.socket.sendall(b'\x03')
                central.read(5, True)
            except Exception as error:
                cleanup_errors.append(('interrupt terminal', str(error)))
        if worker and worker.is_alive():
            try:
                worker_console.socket.sendall(b'\x03')
            except OSError as error:
                cleanup_errors.append(('interrupt worker', str(error)))
            worker.join(15)
        if attached:
            cleanup(central, 'expansion detach dlp-hil')
        if serving:
            cleanup(peripheral, 'dlp stop display0')
        if reverse_attached:
            cleanup(peripheral, 'expansion detach ble-reverse')
        if reverse_serving:
            cleanup(central, 'dlp stop display0')
        if terminal_session:
            cleanup(peripheral, 'session close ' + terminal_session)
        if ftp_started:
            cleanup(central, 'job stop ftpd')
        if uploaded:
            cleanup(central, 'rm -f ' + filename)
        for console in reversed(configured):
            cleanup(console, 'stream ble remove dlp-hil')
        for console in (central, peripheral):
            cleanup(console, 'ble status')
            cleanup(console, 'sessions')
            console.socket.close()
        if cleanup_errors and not failed:
            raise RuntimeError('BLE test cleanup failed: ' + repr(cleanup_errors))


if __name__ == '__main__':
    main()
