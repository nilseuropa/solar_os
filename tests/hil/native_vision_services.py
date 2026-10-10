#!/usr/bin/env python3
"""Run a loadable native ELF against owned image/model fixtures and RTSP video."""
import argparse
import ftplib
import io
import json
from pathlib import Path
import re
import subprocess
import time
from imlib_subset import fixtures
from model_bundle_stream import Console, stop

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--telnet', required=True)
    parser.add_argument('--host-address', required=True)
    parser.add_argument('--mediamtx', type=Path, required=True)
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--legacy-module', type=Path, help='Optional hello ELF built against the original 20-byte application host ABI')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--ftp-port', type=int, default=2121)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    bundle = json.loads((ROOT/'tests/fixtures/model_bundle/raw.json').read_text())
    bundle['model']['file'] = '.native-model.espdl'
    files = {
        '.native-vision.elf': args.module.read_bytes(),
        '.native-scene.png': fixtures()['.imlib-scene.png'],
        '.native-qr.png': (ROOT/'tests/fixtures/vision/multiple.png').read_bytes(),
        '.native-model.espdl': (ROOT/'tests/fixtures/inference/arithmetic_int8.espdl').read_bytes(),
        '.native-bundle.json': json.dumps(bundle).encode(),
    }
    if args.legacy_module: files['.native-legacy.elf'] = args.legacy_module.read_bytes()
    console = None; server = publisher = None
    uploaded = []; models = set(); ftp_started = False; may_load_models = False
    owned_paths = {'/sdcard/dl/.native-model.espdl', '/sdcard/dl/.native-bundle.json'}
    log = (args.output/'console.log').open('w')

    def command(text, seconds=30):
        output = console.command(text, seconds)
        log.write(output); log.flush(); print(output, flush=True)
        return output

    def ftp():
        client = ftplib.FTP(); client.connect(args.telnet, args.ftp_port, timeout=10)
        client.login(); client.voidcmd('TYPE I'); return client

    config = args.output/'server.yml'
    config.write_text('rtsp: true\nrtspAddress: :18754\nrtspTransports: [udp]\n'
                      'rtpAddress: :18200\nrtcpAddress: :18201\nrtmp: false\nhls: false\n'
                      'webrtc: false\nsrt: false\nmoq: false\npaths:\n  all_others:\n    source: publisher\n')
    try:
        with (args.output/'server.log').open('w') as out:
            server = subprocess.Popen([str(args.mediamtx), str(config)], stdout=out, stderr=subprocess.STDOUT)
        time.sleep(1)
        if server.poll() is not None: raise RuntimeError('MediaMTX did not start')
        with (args.output/'publisher.log').open('w') as out:
            publisher = subprocess.Popen(['ffmpeg','-hide_banner','-loglevel','warning','-re','-f','lavfi',
                '-i','testsrc2=size=160x120:rate=5','-c:v','mjpeg','-threads','1','-pix_fmt','yuvj420p',
                '-q:v','6','-huffman','default','-force_duplicated_matrix','1','-an','-f','rtsp',
                '-rtsp_transport','udp','rtsp://127.0.0.1:18754/native-vision'], stdout=out, stderr=subprocess.STDOUT)
        time.sleep(1)
        if publisher.poll() is not None: raise RuntimeError('Publisher did not start')
        console = Console(args.telnet)
        if re.search(r'ftpd[^\r\n]*(?:running|RUNNING)', command('job status')):
            raise RuntimeError('Stop the existing FTP job before this test')
        resident = command('model list')
        if any(line.strip() in owned_paths for line in resident.splitlines()):
            raise RuntimeError('Refusing to reuse an already resident fixture model')
        command('job start ftpd /dl %d' % args.ftp_port); ftp_started = True
        with ftp() as client:
            for name, data in files.items():
                try: client.size('/'+name)
                except ftplib.error_perm as error:
                    if not str(error).startswith('550'): raise
                else: raise RuntimeError('Refusing to overwrite '+name)
                client.storbinary('STOR /'+name, io.BytesIO(data)); uploaded.append(name)
        command('job stop ftpd'); ftp_started = False
        (args.output/'before.log').write_text(command('mem')+command('top')+command('model list'))
        if args.legacy_module:
            output = command('load /dl/.native-legacy.elf ABI')
            (args.output/'legacy.log').write_text(output)
            assert 'Hello from a SolarOS native module, ABI!' in output, 'Original host ABI regression'
        module_command = ('load /dl/.native-vision.elf /sdcard/dl/.native-scene.png /sdcard/dl/.native-qr.png '
                          '/sdcard/dl/.native-model.espdl /sdcard/dl/.native-bundle.json '
                          'rtsp://%s:18754/native-vision' % args.host_address)
        expected = None
        may_load_models = True
        for index in range(3):
            output = command(module_command, 90)
            (args.output/('run-%d.log'%index)).write_text(output)
            found = [int(value) for value in re.findall(r'NATIVE_(?:MODEL|BUNDLE)_HANDLE (\d+)', output)]
            models.update(found)
            assert 'NATIVE_VISION_CHECK_OK' in output, 'ELF validation failed'
            assert 'NATIVE_IMLIB_CANCEL_OK' in output and 'NATIVE_TENSOR_IMAGE_OK' in output
            assert 'NATIVE_QR 2' in output and 'NATIVE_BLOBS 1' in output
            assert output.count('NATIVE_VIDEO_WIDTH 160') == 3 and 'NATIVE_VIDEO_OK' in output
            assert len(found) == 2
            if expected is None: expected = found
            else: assert found == expected, 'Repeated native calls did not reuse resident models'
            for handle in found:
                info = command('model info %d'%handle)
                assert 'handle=%d '%handle in info and 'refs=0' in info
            after = command('mem')+command('top')
            (args.output/('after-%d.log'%index)).write_text(after)
            assert not re.search(r'^(?:vision-imlib|vision-qr|inference|script-rtsp|dl_mc[01])\s', after, re.M)
        for handle in sorted(models):
            output = command('model unload %d'%handle)
            assert 'model:' not in output
        models.clear()
        (args.output/'released.log').write_text(command('mem')+command('top')+command('model list'))
        print('NATIVE_VISION_SERVICES_OK: ELF tables, image/QR/imlib/tensor inference, RTSP, residency and cleanup')
    finally:
        if console is not None:
            try:
                # Also discover a model whose load succeeded before client failure
                # prevented the module from printing its handle. Only our absent
                # before-upload paths are eligible for cleanup.
                resident = command('model list') if may_load_models else ''
                listed_handle = None
                for line in resident.splitlines():
                    match = re.match(r'^(\d+)\s', line)
                    if match: listed_handle = int(match.group(1))
                    if listed_handle and line.strip() in owned_paths: models.add(listed_handle)
                for handle in sorted(models): command('model unload %d'%handle)
                if uploaded:
                    if not ftp_started:
                        command('job start ftpd /dl %d'%args.ftp_port); ftp_started = True
                    with ftp() as client:
                        for name in uploaded: client.delete('/'+name)
                if ftp_started: command('job stop ftpd')
            finally: console.socket.close()
        stop(publisher); stop(server); log.close()


if __name__ == '__main__':
    main()
