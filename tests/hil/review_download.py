#!/usr/bin/env python3
"""Test Playground's staging download and catalog publication on an owned device.

Requires Telnet, Python, Playground and SD storage. Temporarily changes the
Playground source/storage settings and renames existing catalog/alias files to
unique backups. Restores them in finally. Installs only a uniquely named test
package, then checks that a corrupt reinstall leaves its original entry intact.
The evidence directory records every command and all backup paths for recovery
if the device or host disconnects. Do not run alongside another Playground user.
"""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import re
import socket
import threading
import time
import uuid
import zipfile
from model_bundle_stream import Console
from review_correctness import SerialConsole
from bounded_repl import PythonRepl


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--serial', help='Open USB/UART before connecting; saves its boot log')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    token = uuid.uuid4().hex[:8]
    app_id = 'review-' + token
    entry = b"print('REVIEW_DOWNLOAD_ENTRY_OK')\n"
    manifest = dict(id=app_id, version='1.0.0', runtime='python', entry='main.py')
    blob = io.BytesIO()
    with zipfile.ZipFile(blob, 'w', compression=zipfile.ZIP_STORED) as archive:
        archive.writestr('manifest.json', json.dumps(manifest))
        archive.writestr('main.py', entry)
        archive.writestr('payload.bin', bytes(range(256)) * 6144)
    package = blob.getvalue()
    app = dict(manifest, name='Review staging probe', category='review',
               description='Temporary download integrity check', author='Test', tags=[],
               min_solaros='0.0.0', archive='package.sopkg',
               sha256=hashlib.sha256(package).hexdigest(), size=len(package), requires=[])
    catalog = json.dumps(dict(schema='solaros.playground.catalog', schema_version=1,
        categories=[dict(id='review', title='Review', order=0)], apps=[app])).encode()
    state = {'corrupt': False, 'requests': []}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            state['requests'].append(self.path)
            if self.path == '/catalog.json':
                data = catalog
            elif self.path == '/package.sopkg':
                data = package if not state['corrupt'] else package[:-1] + bytes([package[-1] ^ 1])
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = ThreadingHTTPServer(('0.0.0.0', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
        route.connect((socket.gethostbyname(args.host), 23))
        source = f'http://{route.getsockname()[0]}:{server.server_port}/catalog.json'
    serial_console = None
    if args.serial:
        serial_console = SerialConsole(args.serial)
        (args.output / 'boot.log').write_text(serial_console.read(12))
    for attempt in range(10):
        try:
            console = Console(args.host)
            break
        except (OSError, RuntimeError):
            if attempt == 9:
                if serial_console:
                    serial_console.close()
                raise
            time.sleep(1)
    log = (args.output / 'console.log').open('w')
    backups, absent = [], []
    old_source = old_storage = None
    in_python = in_tui = changed = False

    def record(text):
        log.write(text + '\n'); log.flush()
        return text

    def command(text, seconds=30):
        return record(text + '\n' + console.command(text, seconds))

    python = PythonRepl(lambda: console, record).command

    def enter_python():
        nonlocal in_python
        console.socket.sendall(b'python\r\n'); in_python = True
        assert '>>> ' in record(console.read(2))
        python('import solaros; st=solaros.storage')

    def leave_python():
        nonlocal in_python
        console.socket.sendall(b'exit()\r\n'); in_python = False
        record(console.read(2))

    def recovery_record():
        (args.output / 'recovery.json').write_text(json.dumps(dict(
            host=args.host, source=old_source, storage=old_storage,
            backups=backups, absent=absent, app_id=app_id), indent=2) + '\n')

    try:
        assert not re.search(r'^\d+\s+\S+\s+playground\s', command('session list'), re.M)
        old_source = re.search(r'playground source: (\S+)', command('playground source')).group(1)
        old_storage = re.search(r'playground storage: (\w+)', command('playground storage')).group(1)
        enter_python()
        root = '/sdcard/playground/python/' + app_id
        assert 'EXISTS False' in python(f"print('EXISTS',st.exists({root!r}))")
        paths = [mount + '/playground/' + name for mount in ('/sdcard', '/flash')
                 for name in ('catalog.json', 'catalog.source', 'catalog.json.new',
                              'catalog.source.new', 'catalog.json.old', 'catalog.source.old')]
        paths += [mount + '/.shell/' + name for mount in ('/sdcard', '/flash')
                  for name in ('playground', 'playground.new', 'playground.old')]
        for path in paths:
            backup = path + '.review-' + token
            assert 'EXISTS False' in python(f"print('EXISTS',st.exists({backup!r}))")
            if 'EXISTS True' in python(f"print('EXISTS',st.exists({path!r}))"):
                # Record the recovery destination before changing a file.
                backups.append((path, backup)); recovery_record()
                python(f'st.rename({path!r},{backup!r})')
            else:
                absent.append(path); recovery_record()
        leave_python()
        changed = True
        assert 'saved' in command('playground source ' + source)
        assert 'saved' in command('playground storage sd')
        console.socket.sendall(b'playground refresh\r\n'); in_tui = True
        output = ''
        for _ in range(60):
            output += console.read(1)
            if 'catalog refreshed' in output:
                break
        record(output)
        assert 'catalog refreshed' in output, output[-2000:]
        console.socket.sendall(b'q'); in_tui = False
        record(console.read(2))
        assert 'playground: installed ' + app_id in command('playground install ' + app_id + ' sd', 120)
        enter_python()
        python(f'assert st.read_file({root + "/main.py"!r}) == {entry!r}')
        leave_python()
        state['corrupt'] = True
        assert 'ESP_ERR_INVALID_CRC' in command('playground install ' + app_id + ' sd', 120)
        enter_python()
        python(f'assert st.read_file({root + "/main.py"!r}) == {entry!r}')
        leave_python()
        assert state['requests'].count('/package.sopkg') == 2, state
        (args.output / 'result.json').write_text(json.dumps(dict(host=args.host,
            archive_bytes=len(package), stored_entry='pass', corrupt_reinstall='rejected',
            requests=state['requests']), indent=2) + '\n')
    finally:
        if in_tui:
            console.socket.sendall(b'q'); record(console.read(3))
        if in_python:
            leave_python()
        if changed:
            # Remove only the unique fixture install and any of its staging dirs.
            for name in (app_id, '.stage-' + app_id, '.old-' + app_id):
                command('rm -rf /sdcard/playground/python/' + name)
            command('playground source ' + old_source)
            command('playground storage ' + old_storage)
        if backups or absent:
            enter_python()
            for path in absent:
                python(f'st.remove({path!r}) if st.exists({path!r}) else None')
            for path, backup in reversed(backups):
                # A backup may not yet exist if its initial rename failed.
                if 'EXISTS True' in python(f"print('EXISTS',st.exists({backup!r}))"):
                    python(f'st.remove({path!r}) if st.exists({path!r}) else None')
                    python(f'st.rename({backup!r},{path!r})')
            leave_python()
            command('playground reload')
            assert old_source in command('playground source')
            assert 'playground storage: ' + old_storage in command('playground storage')
            (args.output / 'restored.json').write_text(json.dumps(dict(
                source=old_source, storage=old_storage, backup_files=len(backups),
                initially_absent_files=len(absent), restored=True), indent=2) + '\n')
        server.shutdown(); server.server_close()
        log.close(); console.socket.close()
        if serial_console:
            serial_console.close()
    print('DOWNLOAD_STAGE_OK', args.host, flush=True)


if __name__ == '__main__':
    main()
