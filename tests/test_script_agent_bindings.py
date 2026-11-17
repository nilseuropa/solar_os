import base64
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]
CJSON = Path.home() / '.platformio/packages/framework-espidf/components/json/cJSON'
LUA = ROOT / 'components/lua/lua/src'


class ScriptAgentBindingsTest(unittest.TestCase):
    def compile_and_run(self, name, lua=False, output=False):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'test'
            command = [
                'cc', '-std=c11', '-D_GNU_SOURCE', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-no-pie', '-ffunction-sections', '-fdata-sections',
                '-Wl,--gc-sections', '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'src'),
                '-I', str(ROOT / 'src/apps'), '-I', str(ROOT / 'src/services'), '-I', str(CJSON),
                '-idirafter', str(ROOT / 'include'), '-I', str(LUA),
                str(ROOT / f'tests/host/{name}.c'),
                str(ROOT / 'src/services/solar_os_script_agent.c'),
                str(ROOT / 'src/services/solar_os_agent_images.c'),
                str(ROOT / 'src/services/solar_os_media_sample.c'),
                str(ROOT / 'src/services/solar_os_json.c'),
                str(ROOT / 'src/services/solar_os_json_scan.c'), str(CJSON / 'cJSON.c'),
            ]
            if lua:
                command += [str(path) for path in LUA.glob('*.c') if path.name not in {'lua.c', 'luac.c'}]
            command += ['-lm', '-ldl', '-o', str(binary)]
            built = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            bodies = Path(directory) / 'requests.jsonl'
            run = subprocess.run([str(binary)] + ([str(bodies)] if output else []),
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            return [json.loads(line) for line in bodies.read_text().splitlines()] if output else []

    def test_shared_request_adapter_and_image_payloads(self):
        bodies = self.compile_and_run('script_agent_test', output=True)
        self.assertEqual(len(bodies), 2)
        for index, body in enumerate(bodies):
            messages = body['messages'] if index == 0 else body['input']
            parts = next(message['content'] for message in messages if message.get('role') == 'user')
            self.assertEqual(parts[0]['text'], 'Describe both pictures.')
            self.assertEqual(len(parts), 3)
            for part in parts[1:]:
                url = part['image_url']['url'] if index == 0 else part['image_url']
                self.assertTrue(url.startswith('data:image/jpeg;base64,'))
                self.assertEqual(base64.b64decode(url.split(',', 1)[1], validate=True),
                                 bytes([255, 216, 255, 0, 1, 2, 3, 4]))

    def test_python_bindings(self):
        self.compile_and_run('script_agent_python_test')

    def test_audio_files_and_provider_payloads(self):
        bodies = self.compile_and_run('script_agent_audio_test', output=True)
        self.assertEqual(len(bodies), 5)
        wav = b'RIFF' + bytes([44, 0, 0, 0]) + b'WAVEfmt ' + bytes([
            16, 0, 0, 0, 1, 0, 1, 0, 0x80, 0x3e, 0, 0, 0, 0x7d, 0, 0, 2, 0, 16, 0,
        ]) + b'data' + bytes([8, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7])
        mp3 = b'ID3' + bytes([4, 0, 0, 0, 0, 0, 0, 0xff, 0xfb, 0x90, 0])
        for body, expected, encoding in zip(bodies, [wav, mp3, wav, wav + bytes(1024 * 1024 - len(wav))],
                                            ['wav', 'mp3', 'wav', 'wav']):
            self.assertEqual(body['modalities'], ['text'])
            self.assertNotIn('reasoning_effort', body)
            parts = next(message['content'] for message in body['messages'] if message.get('role') == 'user')
            self.assertEqual(parts[0]['text'], 'Summarize this recording.')
            self.assertEqual(parts[1]['type'], 'input_audio')
            self.assertEqual(parts[1]['input_audio']['format'], encoding)
            self.assertEqual(base64.b64decode(parts[1]['input_audio']['data'], validate=True), expected)
        self.assertEqual(bodies[2]['messages'][-1]['role'], 'tool')
        self.assertEqual(bodies[4]['modalities'], ['text'])
        self.assertNotIn('reasoning_effort', bodies[4])
        self.assertEqual(bodies[4]['messages'][-1]['content'], 'Summarize your previous answer.')

    def test_lua_bindings_with_real_interpreter(self):
        self.compile_and_run('script_agent_lua_test', lua=True)

    def test_native_cancel_poll_calls_script_outside_service_lock(self):
        source = (ROOT / 'src/services/solar_os_agent.c').read_text()
        poll = production_functions(source, ('solar_os_agent_provider_cancel_requested',))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
typedef bool (*solar_os_agent_cancel_fn)(void *);
static int critical;
#define portENTER_CRITICAL(lock) ((void)(lock), critical++)
#define portEXIT_CRITICAL(lock) ((void)(lock), critical--)
static int agent_lock;
static struct {
    bool cancel_requested;
    solar_os_agent_cancel_fn should_cancel;
    void *cancel_user_data;
} agent;
static bool script_cancel(void *user) {
    assert(critical == 0);
    return *(bool *)user;
}
'''
        harness += poll + r'''
int main(void) {
    assert(!solar_os_agent_provider_cancel_requested());
    bool cancel = false;
    agent.should_cancel = script_cancel;
    agent.cancel_user_data = &cancel;
    assert(!solar_os_agent_provider_cancel_requested());
    cancel = true;
    assert(solar_os_agent_provider_cancel_requested());
    cancel = false;
    agent.cancel_requested = true;
    assert(solar_os_agent_provider_cancel_requested());
    assert(critical == 0);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'cancel.c'
            path.write_text(harness)
            binary = Path(directory) / 'cancel'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
