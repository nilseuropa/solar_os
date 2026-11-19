import base64
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CJSON = Path.home() / '.platformio/packages/framework-espidf/components/json/cJSON'


class AgentImagesTest(unittest.TestCase):
    def test_encoded_sources_and_optional_services(self):
        for rtsp, pipeline in ((0, 0), (1, 0), (1, 1)):
            with self.subTest(rtsp=rtsp, pipeline=pipeline), tempfile.TemporaryDirectory() as directory:
                binary = Path(directory) / 'sources'
                subprocess.run([
                    'cc', '-std=c11', '-D_GNU_SOURCE', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-no-pie',
                    f'-DSOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT={rtsp}', f'-DSOLAR_OS_PACKAGE_SERVICE_PIPELINE={pipeline}',
                    '-I', str(ROOT / 'tests/host/script_media_stubs'), '-I', str(ROOT / 'tests/host/rtsp_stubs'),
                    '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                    '-idirafter', str(ROOT / 'include'), str(ROOT / 'tests/host/media_sample_source_test.c'),
                    str(ROOT / 'src/services/solar_os_media_sample.c'),
                    str(ROOT / 'src/services/solar_os_media_sample_source.c'), '-o', str(binary),
                ], check=True)
                subprocess.run([str(binary)], check=True, timeout=15)

    def test_native_image_requests_and_ownership(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'images'
            output = Path(directory) / 'requests.jsonl'
            subprocess.run([
                'cc', '-std=c11', '-D_GNU_SOURCE', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-no-pie', '-ffunction-sections', '-fdata-sections',
                '-Wl,--gc-sections', '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'src'),
                '-I', str(ROOT / 'src/services'), '-I', str(CJSON), '-idirafter', str(ROOT / 'include'),
                str(ROOT / 'tests/host/agent_images_test.c'),
                str(ROOT / 'src/services/solar_os_agent_images.c'),
                str(ROOT / 'src/services/solar_os_media_sample.c'),
                str(ROOT / 'src/services/solar_os_json.c'),
                str(ROOT / 'src/services/solar_os_json_scan.c'), str(CJSON / 'cJSON.c'),
                '-lm', '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary), str(output)], check=True, timeout=30)
            bodies = [json.loads(line) for line in output.read_text().splitlines()]
            self.assertEqual(len(bodies), 7)
            for index, body in enumerate(bodies[:4]):
                messages = body['messages'] if index < 2 else body['input']
                user = next(message for message in messages if message.get('role') == 'user')
                parts = user['content']
                self.assertEqual(parts[0]['text'], 'What is "here"?\n\t')
                for image, expected in zip(parts[1:], [b'\xff\xd8\xff\0\1', b'\x89PNG\r\n\x1a\n']):
                    url = image['image_url']['url'] if index < 2 else image['image_url']
                    self.assertEqual(base64.b64decode(url.split(',', 1)[1], validate=True), expected)
            self.assertEqual(bodies[4]['input'][0]['type'], 'function_call_output')
            self.assertNotIn('data:image/', json.dumps(bodies[4]))
            self.assertIsInstance(bodies[5]['input'][0]['content'], str)
            large = bodies[6]['input'][0]['content'][1]['image_url']
            expected = bytearray(index % 256 for index in range(512 * 1024))
            expected[:3] = b'\xff\xd8\xff'
            self.assertEqual(base64.b64decode(large.split(',', 1)[1], validate=True), expected)
