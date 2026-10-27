#!/usr/bin/env python3
"""Validate libraries using a signed maintainer catalog with the three examples.

Requires hello-library, library-check and library-user in the configured catalog.
Owns those test installs only, refuses to replace pre-existing modules, and
restarts the supplied device to check cold boot. Does not publish a catalog.
"""
import argparse
import json
from pathlib import Path
import re
import time

from model_bundle_stream import Console


def reconnect(host):
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        try:
            return Console(host)
        except (OSError, RuntimeError):
            time.sleep(1)
    raise RuntimeError("device did not return after restart")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--telnet', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runs', type=int, default=10)
    args = parser.parse_args()
    if not 1 <= args.runs <= 100:
        parser.error('--runs must be 1..100')
    args.output.mkdir(parents=True, exist_ok=False)
    console = reconnect(args.telnet)
    owned = []
    log = (args.output/'console.log').open('w')

    def command(text, seconds=30):
        out = console.command(text, seconds)
        log.write(out); log.flush(); print(out, flush=True)
        return out

    def install(name):
        out = command('pkg install ' + name, 60)
        assert 'pkg: installed' in out, out
        owned.append(name)

    def check(shared=False):
        out = command('library-check' + (' shared' if shared else ''), 60)
        assert 'LIBRARY_CHECK_OK 25 cycles' in out and 'FAILED' not in out, out

    def dormant():
        out = command('pkg installed')
        assert 'library hello-library (on storage)' in out, out

    try:
        installed = command('pkg installed')
        for name in ('hello-library', 'library-check', 'library-user'):
            assert not re.search(r'^(?:app|job|library) ' + name + r'\b', installed, re.M), installed
        command('board'); command('version')
        available = command('pkg available', 60)
        assert 'hello-library' in available and 'library-user' in available, available
        install('hello-library'); dormant()
        install('library-check'); install('library-user'); dormant()
        check()
        before = command('mem')
        for _ in range(args.runs):
            check()
        after = command('mem')
        assert 'job started: library-user' in command('job start library-user')
        time.sleep(1)
        status = command('job status library-user')
        assert 'running' in status.lower(), status
        assert 'library hello-library (loaded)' in command('pkg installed')
        for text in ('pkg unload hello-library', 'pkg remove hello-library'):
            out = command(text)
            assert 'library is in use' in out, out
        out = command('pkg install hello-library', 60)
        assert 'active resident module' in out, out
        check(shared=True)
        assert 'running' in command('job status library-user').lower()
        command('job stop library-user')
        assert 'pkg: unloaded library hello-library' in command('pkg unload hello-library')
        dormant()
        # Reboot with all three installed: registering the job must not eagerly
        # load its library. The checker must still resolve the installed ELF.
        console.socket.sendall(b'reboot\r\n')
        console.socket.close()
        time.sleep(2)
        console = reconnect(args.telnet)
        dormant(); check()
        results = {'load_unload_cycles': 25 * (args.runs + 2),
                   'shared_consumer_cycles': 25,
                   'signed_install': True, 'boot_lazy': True,
                   'busy_unload_remove_reinstall': True, 'before': before, 'after': after}
        (args.output/'results.json').write_text(json.dumps(results, indent=2)+'\n')
        print('NATIVE_LIBRARIES_OK', flush=True)
    finally:
        try:
            if 'library-user' in owned:
                command('job stop library-user')
            for name in reversed(owned):
                out = command('pkg remove ' + name)
                assert 'pkg: removed ' + name in out, out
            command('pkg installed'); command('mem')
        finally:
            log.close(); console.socket.close()


if __name__ == '__main__':
    main()
