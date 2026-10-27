#!/usr/bin/env python3
"""Check managed PSRAM using supplied classifier and owned guest fixtures.

Requires installed espdl/Apple II/Game Boy/Tiny386 modules and an idle display
shell. The fixture directory must contain classify.py, labels.txt, its ImageNet
model, test.ppm, test.gb, dos.img, and launch-APP.sh scripts. Each launch
script waits two seconds then creates its guest on display0. Does not flash,
install, remove files, or close pre-existing sessions. Reclaims unused libraries, including espdl.
"""
import argparse
import json
from pathlib import Path
import re
import time

from model_bundle_stream import Console


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--directory', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    console = Console(args.host)
    opened = []
    loaded = None
    with (args.output / 'console.log').open('w') as log:
        def command(text, seconds=30):
            out = console.command(text, seconds)
            log.write(out); log.flush()
            # Do not emit terminal horizontal-scroll redraws or bearer tokens.
            print('\n'.join(line for line in out.splitlines()
                  if len(line) < 250 and 'access code:' not in line), flush=True)
            return out

        def sessions():
            out = command('session list')
            states = {}
            for sid, app in opened:
                match = re.search(r'^' + sid + r'\s+.*?\b' + app +
                                  r'\s+(active|suspended)\s+display0\b', out, re.M)
                if not match:
                    raise RuntimeError('guest session disappeared: ' + app)
                states[app] = match[1]
            return states

        def create(app):
            nonlocal console
            # Launch from the display shell after the prior guest releases its
            # presenter/audio claims, matching normal on-device use.
            for _ in range(len(opened) + 2):
                out = command('session list')
                if re.search(r'^0\s+shell\s+shell\s+active\s+display0\b', out, re.M):
                    break
                command('input emit ALT+TAB'); time.sleep(.3)
            else:
                raise RuntimeError('display shell could not be selected')
            command('mem')
            text = 'sh ' + args.directory.rstrip('/') + '/launch-' + app + '.sh'
            for char in text:
                key = 'SPACE' if char == ' ' else char
                command('input emit ' + key)
                time.sleep(.04)
            # Emit Enter without waiting for the display script. Its delay lets
            # the observing telnet shell release its 16 KiB task stack before
            # the guest's SRAM admission check.
            console.socket.sendall(b'input emit ENTER\r\n')
            time.sleep(.1)
            console.socket.close()
            time.sleep(4)
            console = Console(args.host)
            out = command('session list')
            match = re.search(r'^(\d+)\s+.*?\b' + app +
                              r'\s+active\s+display0\b', out, re.M)
            if not match:
                raise RuntimeError('guest creation failed: ' + app)
            opened.append((match[1], app))
            if sessions()[app] != 'active':
                raise RuntimeError('guest did not remain active: ' + app)

        try:
            initial = command('session list')
            if re.search(r'^\d+\s+.*\b(?:apple2|gameboy|tiny386)\b', initial, re.M):
                raise RuntimeError('guest sessions already exist')
            if 'No resident models' not in command('model list'):
                raise RuntimeError('models already resident')
            command('mem'); command('mem reclaim')
            base = args.directory.rstrip('/')
            out = command('model load ' + base + '/imagenet_cls_mobilenetv2_s8_v1.espdl 60000', 70)
            match = re.search(r'Resident model (\d+)', out)
            if not match:
                raise RuntimeError('resident model load failed')
            loaded = match[1]
            command('model info ' + loaded)
            held = command('mem reclaim')
            if 'Released 0 unused libraries' not in held or 'library espdl (loaded)' not in command('pkg installed'):
                raise RuntimeError('active provider was reclaimed')
            command('model unload ' + loaded); loaded = None
            command('mem reclaim')
            results = []
            for mode in ('single', 'auto', 'dual'):
                out = command('python ' + base + '/classify.py ' + base +
                              '/test.ppm --json --repeat 2 --mode ' + mode, 120)
                rows = [json.loads(line) for line in out.splitlines() if line.startswith('{')]
                if len(rows) != 1 or 'Traceback' in out or 'ESP_ERR_' in out:
                    raise RuntimeError('classification failed: ' + mode)
                if len(rows[0]['top']) != 5:
                    raise RuntimeError('incomplete classification')
                results.append(rows[0])
                command('mem'); command('mem reclaim')
            if any(row['top'] != results[0]['top'] for row in results):
                raise RuntimeError('classification differs across execution modes')

            # Retain all three guest states, then cycle through each repeatedly.
            # Start the largest declared requirement before retaining other guests.
            create('tiny386')
            create('gameboy')
            create('apple2')
            before = sessions()
            command('mem reclaim')
            if sessions() != before:
                raise RuntimeError('reclamation changed guest sessions')
            seen = {app: 0 for _, app in opened}
            for _ in range(12):
                command('input emit ALT+TAB')
                time.sleep(1)
                for app, state in sessions().items():
                    if state == 'active': seen[app] += 1
            if any(count < 2 for count in seen.values()):
                raise RuntimeError('not every guest resumed repeatedly: ' + str(seen))
            command('mem'); command('top')
            (args.output / 'results.json').write_text(json.dumps({
                'classification': results, 'guest_resumes': seen,
                'held_library_protected': True, 'guest_state_reclamation_protected': True,
            }, indent=2) + '\n')
            print('PSRAM_LIFETIMES_OK', flush=True)
        finally:
            for sid, _ in reversed(opened): command('session close ' + sid)
            if loaded: command('model unload ' + loaded)
            command('mem reclaim'); command('mem'); command('session list')
            console.socket.close()


if __name__ == '__main__':
    main()
