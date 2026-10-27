from pathlib import Path
import ast
import re
import shutil
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DisplaydColorTest(unittest.TestCase):
    def test_http_snapshot_lifetime_and_payloads(self):
        source = (ROOT / "src/jobs/solar_os_displayd_job.c").read_text()
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "displayd_raw.inc").write_text(production_functions(source, (
                "displayd_send_raw_frame", "displayd_raw_handler", "displayd_rgb_handler")))
            binary = Path(tmp, "displayd_raw")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", tmp, str(ROOT / "tests/host/displayd_color_http_test.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    @unittest.skipUnless(shutil.which("node"), "requires Node.js")
    def test_browser_color_and_monochrome_rotation(self):
        source = (ROOT / "src/jobs/solar_os_displayd_job.c").read_text()
        page = source.split("static const char display_page[] =", 1)[1].split(
            "static esp_err_t displayd_send_503", 1)[0]
        html = "".join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', page))
        script = html.split("<script>", 1)[1].split("</script>", 1)[0]
        harness = r"""
const assert = require('node:assert/strict');
let rendered;
const fakeContext = {
  createImageData: (width, height) => ({width, height, data: new Uint8ClampedArray(width*height*4)}),
  putImageData: im => { rendered = im; }
};
const elements = Object.fromEntries(['#token', '#connect', '#screen', '#status'].map(id =>
  [id, {value: '123456', getContext: () => fakeContext, addEventListener(){}, focus(){}}]));
const document = {querySelector: id => elements[id], addEventListener(){}};
const setTimeout = () => 0, clearTimeout = () => {};
let requests = [];
let metadata;
const fetch = async url => {
  requests.push(url);
  return url === '/api/displays' ? {ok: true, json: async () => ({displays: [metadata]})} :
    {status: 304};
};
"""
        checks = r"""
const native = [0xf800, 0x07e0, 0x001f, 0, 0xffff, 0x18c3];
const permutations = [[0,1,2,3,4,5], [2,5,1,4,0,3], [5,4,3,2,1,0], [3,0,4,1,5,2]];
const expected = [[255,0,0], [0,255,0], [0,0,255], [0,0,0], [255,255,255], [25,24,25]];
const bytes = Uint8Array.from(native.flatMap(c => [c >> 8, c & 255]));
for (let rotation = 0; rotation < 4; rotation++) {
  for (const black_is_one of [true, false]) {
    const m = {native_width: 3, native_height: 2, native_stride: 8, rotation, black_is_one,
      width: rotation & 1 ? 2 : 3, height: rotation & 1 ? 3 : 2, pixel_format: 'rgb565'};
    raw(bytes, m);
    permutations[rotation].forEach((n, i) => {
      assert.deepEqual(Array.from(rendered.data.slice(i*4, i*4+4)), [...expected[n], 255]);
    });
    // A monochrome frame uses native vertical tiles and the declared polarity.
    const mono = Uint8Array.from([1,2,1,0,0,0,0,0]);
    raw(mono, {...m, pixel_format: 'mono1'});
    permutations[rotation].forEach((n, i) => {
      const black = ((n % 2) === 0) === black_is_one;
      const v = black ? 0 : 255;
      assert.deepEqual(Array.from(rendered.data.slice(i*4, i*4+4)), [v,v,v,255]);
    });
  }
}
(async () => {
  for (const pixel_format of ['rgb565', 'mono1']) {
    requests = [];
    metadata = {pixel_format, frame_rgb565: '/color', frame_raw: '/mono', input: '/input'};
    await connect();
    await Promise.resolve();
    assert.deepEqual(requests, ['/api/displays', pixel_format === 'rgb565' ? '/color' : '/mono']);
  }
  console.log('displayd browser: exact color, rotation, mono polarity and endpoint selection passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
"""
        subprocess.run(["node", "-e", harness + script + checks], check=True)


if __name__ == "__main__":
    unittest.main()
