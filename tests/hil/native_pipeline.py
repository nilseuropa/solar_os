#!/usr/bin/env python3
"""Check native QR/model jobs, interpreter survival, RTSP and explicit teardown.

Requires installed /dl/models/{imagenet,pedestrian}/bundle.json, /dl/cat.png,
/dl/pedestrian.png and enabled Telnet/FTP jobs. Owns only its uploaded temporary
fixtures, pipeline IDs, resident models, and private host publisher processes.
"""
import argparse
import ftplib
import json
from pathlib import Path
import re
import socket
import subprocess
import time
from model_bundle_stream import Console, stop

ROOT = Path(__file__).resolve().parents[2]
FILES = {
    '/dl/.pipeline-client.py': ROOT/'tests/hil/pipeline_client.py',
    '/dl/.pipeline-client.lua': ROOT/'tests/hil/pipeline_client.lua',
    '/dl/.pipeline-qr.png': ROOT/'tests/fixtures/vision/qr.png',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--telnet', required=True)
    parser.add_argument('--host-address', required=True)
    parser.add_argument('--mediamtx', required=True)
    parser.add_argument('--ftp-port', type=int, default=2121)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    console = Console(args.telnet)
    models, pipelines, uploaded = [], [], []
    server = publisher = None
    server_log = publisher_log = None
    log = (args.output/'console.log').open('w')
    large_qr = args.output/'qr-large.png'
    subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-i',str(FILES['/dl/.pipeline-qr.png']),
        '-vf','scale=792:792:flags=neighbor','-frames:v','1',str(large_qr)],check=True)
    fixtures = dict(FILES)
    fixtures['/dl/.pipeline-qr-large.png'] = large_qr

    def command(text, seconds=30):
        out = console.command(text, seconds)
        log.write(out); log.flush(); print(out, flush=True)
        return out

    def load(name):
        out = command('model bundle /dl/models/%s/bundle.json' % name, 90)
        match = re.search(r'Resident model (\d+)', out)
        assert match, out
        handle = int(match[1]); models.append(handle); return handle

    def start(source, processor, model, limit=0, interval=0):
        out = command('python /dl/.pipeline-client.py start %s %s %d %d %d' %
                      (source, processor, model, limit, interval))
        match = re.search(r'PIPELINE_STARTED (\d+)', out); assert match, out
        p = int(match[1]); pipelines.append(p); return p

    def inspect(p):
        out = command('python /dl/.pipeline-client.py inspect %d' % p)
        assert 'PIPELINE_PYTHON_OK' in out, out
        lines = [json.loads(line) for line in out.splitlines() if line.startswith('{')]
        assert len(lines) == 1, out
        return lines[0]

    def finish(p):
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            out = command('pipeline status %d' % p)
            if 'done=true' in out:
                assert 'error=ESP_OK' in out, out
                return inspect(p)
            time.sleep(.25)
        raise RuntimeError('pipeline did not finish')

    def remove(p):
        out = command('pipeline destroy %d' % p)
        assert 'pipeline:' not in out, out
        pipelines.remove(p)

    try:
        with ftplib.FTP() as ftp:
            ftp.connect(args.telnet, args.ftp_port, timeout=10); ftp.login()
            for target, local in fixtures.items():
                remote = target.removeprefix('/dl')
                try:
                    ftp.size(remote)
                except ftplib.error_perm as error:
                    assert str(error).startswith('550'), error
                else:
                    raise RuntimeError('Refusing to overwrite '+target)
                with local.open('rb') as source:
                    ftp.storbinary('STOR '+remote, source)
                uploaded.append(target)
        command('job stop ftpd')
        before = command('mem') + command('top')
        (args.output/'before.log').write_text(before)
        qr = start('/dl/.pipeline-qr.png', 'qr', 0, 3, 500)
        value = finish(qr)
        assert value['status']['frames'] == 3
        assert value['latest']['result']['codes'][0]['payload_hex'] == '536f6c61724f532051520062696e617279ff'
        assert 'PIPELINE_LUA_OK' in command('lua /dl/.pipeline-client.lua')
        remove(qr)
        qr = start('/dl/.pipeline-qr-large.png', 'qr', 0)
        scaled = finish(qr)['latest']['result']
        assert scaled['width'] == scaled['height'] == 792
        assert scaled['processed_width'] == scaled['processed_height'] == 480
        assert scaled['codes'][0]['payload_hex'] == '536f6c61724f532051520062696e617279ff'
        assert max(x for x,y in scaled['codes'][0]['corners']) > 640
        remove(qr)
        classifier = load('imagenet')
        p = start('/dl/cat.png', 'model', classifier)
        classified = finish(p)
        assert classified['latest']['result']['result']['classes'][0]['label'] == 'tabby'
        assert 'PIPELINE_LUA_OK' in command('lua /dl/.pipeline-client.lua')
        remove(p)
        command('model unload %d' % classifier); models.remove(classifier)
        detector = load('pedestrian')
        p = start('/dl/pedestrian.png', 'model', detector, 3)
        detected = finish(p)
        assert len(detected['latest']['result']['result']['detections']) == 3
        assert 'PIPELINE_LUA_OK' in command('lua /dl/.pipeline-client.lua')
        remove(p)
        for port, kind in ((18654,socket.SOCK_STREAM),(18100,socket.SOCK_DGRAM),(18101,socket.SOCK_DGRAM)):
            with socket.socket(socket.AF_INET,kind) as probe: probe.bind(('0.0.0.0',port))
        config = args.output/'server.yml'
        config.write_text('readTimeout: 30s\nrtsp: true\nrtspAddress: :18654\nrtspTransports: [udp]\n'
            'rtpAddress: :18100\nrtcpAddress: :18101\nrtmp: false\nhls: false\nwebrtc: false\nsrt: false\nmoq: false\n'
            'paths:\n  all_others:\n    source: publisher\n')
        server_log = (args.output/'server.log').open('w')
        publisher_log = (args.output/'publisher.log').open('w')
        server = subprocess.Popen([args.mediamtx,str(config)],stdout=server_log,stderr=subprocess.STDOUT)
        time.sleep(1); assert server.poll() is None
        publisher = subprocess.Popen(['ffmpeg','-hide_banner','-loglevel','warning','-re','-f','lavfi',
            '-i','testsrc2=size=160x120:rate=10','-c:v','mjpeg','-threads','1','-pix_fmt','yuvj420p',
            '-q:v','6','-huffman','default','-force_duplicated_matrix','1','-an','-f','rtsp',
            '-rtsp_transport','udp','rtsp://127.0.0.1:18654/native-pipeline'],stdout=publisher_log,stderr=subprocess.STDOUT)
        time.sleep(2); assert publisher.poll() is None
        p = start('rtsp://%s:18654/native-pipeline' % args.host_address, 'model', detector)
        samples = []
        for _ in range(6):
            time.sleep(1); samples.append(inspect(p))
        sequences = [s['latest']['sequence'] for s in samples]
        timestamps = [s['latest']['source_timestamp_us'] for s in samples]
        assert all(a < b for a,b in zip(sequences,sequences[1:])), sequences
        assert all(a < b for a,b in zip(timestamps,timestamps[1:])), timestamps
        for s in samples:
            transform = next(iter(s['latest']['result']['transforms'].values()))
            assert transform['source_width'] == 160 and transform['source_height'] == 120
        assert 'PIPELINE_LUA_OK' in command('lua /dl/.pipeline-client.lua')
        blocked = command('model unload %d' % detector)
        assert 'model:' in blocked and ('busy' in blocked.lower() or 'in use' in blocked.lower()), blocked
        t = time.monotonic(); stopped = command('pipeline stop %d' % p)
        stop_seconds = time.monotonic() - t
        assert 'pipeline:' not in stopped, stopped
        assert 'refs=0' in command('model info %d' % detector)
        after = command('mem') + command('top')
        assert not re.search(r'^(?:pipeline-\d+|inference|script-rtsp|dl_mc[01])\s', after, re.M), after
        (args.output/'stopped.log').write_text(after)
        remove(p)
        command('model unload %d' % detector); models.remove(detector)
        (args.output/'results.json').write_text(json.dumps({'qr':value,'scaled_qr':scaled,'classifier':classified,'detector':detected,
            'stream':samples,'stop_console_seconds':stop_seconds},indent=2)+'\n')
        command('pipeline list'); command('model list'); command('mem')
        print('NATIVE_PIPELINE_OK: QR binary payload, classifier, detector, interpreter survival, RTSP, retention, stop and cleanup')
    finally:
        for p in pipelines:
            try: command('pipeline destroy %d' % p)
            except (OSError,RuntimeError): pass
        for handle in models:
            try: command('model unload %d' % handle)
            except (OSError,RuntimeError): pass
        stop(publisher); stop(server)
        if server_log: server_log.close()
        if publisher_log: publisher_log.close()
        if uploaded:
            command('job start ftpd /dl %d' % args.ftp_port)
            with ftplib.FTP() as ftp:
                ftp.connect(args.telnet,args.ftp_port,timeout=10); ftp.login()
                for path in uploaded: ftp.delete(path.removeprefix('/dl'))
            command('job stop ftpd')
        log.close(); console.socket.close()


if __name__ == '__main__':
    main()
