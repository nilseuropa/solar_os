from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TINYCC = ROOT / 'modules/experimental/tinycc'
sys.path.insert(0, str(TINYCC))
from native_build import BANNER, embedded_header, pack_sdk_headers, pack_standard_runtime


class PackedCcRuntimeTest(unittest.TestCase):
    def test_packed_headers_preserve_complete_and_partial_reads(self):
        headers = {path.name: embedded_header(path) for path in (ROOT / 'include').glob('solar_os_native*.h')}
        headers['empty.h'] = b''
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / 'sdk_packed.inc').write_text(pack_sdk_headers(headers))
            binary = directory / 'check'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-no-pie', '-I', str(TINYCC), '-I', tmp,
                '-x', 'c', '-', '-o', str(binary)], input=r'''
#include <assert.h>
#include <stdio.h>
#include "sdk_packed.h"

int main(void)
{
    uint8_t file[32768], part[41];
    for (size_t i = 0; i < sizeof(sdk) / sizeof(sdk[0]); ++i) {
        const solar_os_cc_embedded_header_t *header = &sdk[i];
        assert(header->bytes <= sizeof(file));
        for (size_t start = 0; start < header->bytes; start += SOLAR_OS_CC_SDK_BLOCK_BYTES) {
            size_t size = header->bytes - start;
            if (size > SOLAR_OS_CC_SDK_BLOCK_BYTES) size = SOLAR_OS_CC_SDK_BLOCK_BYTES;
            assert(solar_os_cc_read_embedded_header(header, start, file + start, size));
        }
        for (size_t start = 0; start < header->bytes; start += 117) {
            size_t size = header->bytes - start;
            if (size > sizeof(part)) size = sizeof(part);
            assert(solar_os_cc_read_embedded_header(header, start, part, size));
            assert(!memcmp(part, file + start, size));
        }
        assert(!solar_os_cc_read_embedded_header(header, header->bytes, part, 1));
        assert(fwrite(file, 1, header->bytes, stdout) == header->bytes);
    }
}
''', text=True, check=True)
            expected = b''.join(headers[name] for name in sorted(headers))
            self.assertEqual(subprocess.check_output([str(binary)]), expected)

    def test_compact_generated_headers_preserve_preprocessor_behavior(self):
        content = BANNER + '''
/* Full contract prose remains in the public SDK.
 * Preserve the position of the lines below. */
#define SDK_VERSION 1 /* an inline comment remains */
const char *marker = "/* a literal remains */";
#define SDK_MULTILINE \\
    /* a non-standalone preprocessor comment\n     * remains with the splice */ 42
const int line = __LINE__;
const int version = SDK_VERSION;
const int multiline = SDK_MULTILINE;
'''
        with tempfile.TemporaryDirectory() as tmp:
            header = Path(tmp) / 'generated.h'
            compact = Path(tmp) / 'compact.h'
            header.write_text(content)
            compact.write_bytes(embedded_header(header))
            header.write_text(content.removeprefix(BANNER))
            compact_text = compact.read_text()
            self.assertNotIn('Full contract prose', compact_text)
            self.assertIn('an inline comment remains', compact_text)
            self.assertIn('/* a literal remains */', compact_text)
            self.assertIn('a non-standalone preprocessor comment', compact_text)
            command = ['cc', '-E', '-P', '-x', 'c']
            self.assertEqual(subprocess.check_output([*command, str(header)]),
                             subprocess.check_output([*command, str(compact)]))

    def test_standard_source_decodes_into_the_compiler_buffer(self):
        original = TINYCC / 'sdk_runtime.c'
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / 'sdk_runtime.inc').write_text(pack_standard_runtime(original))
            source = directory / 'unpack.c'
            source.write_text('''
#include <assert.h>
#include <stdio.h>
#include "sdlp_lz4_block.h"
#include "sdk_runtime.inc"

int main(void)
{
    uint8_t source[SOLAR_OS_CC_STANDARD_SOURCE_BYTES + 1];
    uint8_t *sentinel = source + SOLAR_OS_CC_STANDARD_SOURCE_BYTES;
    *sentinel = 0xa5;
    assert(solar_os_lz4_decode_block(source, SOLAR_OS_CC_STANDARD_SOURCE_BYTES,
        standard_runtime, sizeof(standard_runtime)));
    assert(*sentinel == 0xa5);
    return fwrite(source, 1, SOLAR_OS_CC_STANDARD_SOURCE_BYTES, stdout) !=
        SOLAR_OS_CC_STANDARD_SOURCE_BYTES;
}
''')
            binary = directory / 'unpack'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'protocol/display_link/include'), str(source),
                '-o', str(binary)], check=True)
            self.assertEqual(subprocess.check_output([str(binary)]), original.read_bytes())
