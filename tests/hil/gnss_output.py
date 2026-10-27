#!/usr/bin/env python3
"""Opt-in T-Deck GNSS output/ownership HIL over an existing telnet shell.

Requires firmware with detachable board gnss0 and software output controls.
Captures raw NMEA without requiring a position fix. Restores board attachment
and output on exit. Does not flash firmware or test electrical TX isolation.
"""
import argparse
import json
from pathlib import Path
import re
import time

from model_bundle_stream import Console


def valid_sentences(raw):
    result = []
    for body, checksum in re.findall(r'\$([^*$\r\n]*)\*([0-9A-Fa-f]{2})', raw):
        if not body.isascii():
            continue
        value = 0
        for byte in body.encode():
            value ^= byte
        if value == int(checksum, 16):
            result.append(body.split(','))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--telnet', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bindings', action='store_true', help='Also test Python and Lua bindings')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    console = Console(args.telnet)
    transcript = []
    detached = False

    def command(text, timeout=12):
        result = console.command(text, timeout)
        transcript.append(result)
        print(result, flush=True)
        return result

    def capture(label, seconds):
        nonlocal detached
        assert 'detached gnss0' in command('expansion detach gnss0')
        detached = True
        raw = bytearray()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            dump = console.command('uart read uart0 250', 3)
            transcript.append(dump)
            for line in dump.splitlines():
                match = re.match(r'^[0-9a-f]{4}:((?: [0-9a-f]{2}){1,16})', line)
                if match:
                    raw.extend(bytes.fromhex(match.group(1)))
        (args.output / (label + '.bin')).write_bytes(raw)
        raw = raw.decode(errors='replace')
        (args.output / (label + '.txt')).write_text(raw)
        assert 'attached gnss0 (board)' in command('expansion attach gnss0')
        detached = False
        return raw

    try:
        command('version')
        assert 't_deck_plus' in command('board')
        command('expansion devices')
        layout = command('expansion layout GROVE')
        assert 'INTERNAL' not in layout and 'GROVE' in layout
        assert 'output=enabled' in command('gnss status gnss0')
        before = capture('before', 4)
        before_sentences = valid_sentences(before)
        assert any(fields[0].endswith('RMC') for fields in before_sentences), before
        assert 'output=disabled' in command('gnss disable gnss0')
        status = command('gnss status gnss0')
        assert 'power=always-on output=disabled fix=unavailable' in status
        assert 'ESP_ERR_INVALID_STATE' in command('gnss fix gnss0')
        assert 'Owner: gnss0' in command('uart status uart0')
        quiet = capture('disabled', 4)
        # Reattachment automatically resumes the muted receiver.
        assert quiet == '', quiet
        assert 'output=enabled' in command('gnss status gnss0')
        assert 'output=enabled' in command('gnss enable gnss0')
        after = capture('after', 4)
        after_sentences = valid_sentences(after)
        assert any(fields[0].endswith('RMC') for fields in after_sentences), after
        bindings = []
        if args.bindings:
            for language, source, marker in (
                ('python', "import solaros; assert solaros.gnss.disable('gnss0') is False; "
                 "assert solaros.gnss.enable('gnss0') is True; print('GNSS_PYTHON_OK')",
                 'GNSS_PYTHON_OK'),
                ('lua', "assert(solaros.gnss.disable('gnss0') == false); "
                 "assert(solaros.gnss.enable('gnss0') == true); print('GNSS_LUA_OK')",
                 'GNSS_LUA_OK'),
            ):
                result = command('agent script ' + language + ' -c "' + source + '"', 25)
                assert re.search(r'^' + marker + r'\r?$', result, re.M), result
                bindings.append(language)
        summary = {
            'result': 'GNSS_OUTPUT_HIL_PASS',
            'rmc_statuses': [fields[2] for fields in before_sentences + after_sentences
                             if fields[0].endswith('RMC')],
            'quiet_capture_seconds': 4,
            'electrical_isolation_tested': False,
            'bindings': bindings,
        }
        (args.output / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
        print(json.dumps(summary), flush=True)
    finally:
        try:
            if detached:
                command('expansion attach gnss0')
            command('gnss enable gnss0')
        finally:
            (args.output / 'console.log').write_text('\n'.join(transcript))
            console.socket.close()


if __name__ == '__main__':
    main()
