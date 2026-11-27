#!/usr/bin/env python3
"""Generate a lossless, smaller build copy of the serial flasher stubs."""

import argparse
from pathlib import Path
import re
import struct
import zlib


ARRAY = re.compile(r'(static const uint8_t (\w+)\[\] = \{)([^}]+)(\};)')
BYTE = re.compile(r'0x[0-9a-fA-F]{2}')
INCLUDE = '#include "esp_stubs.h"'

SERIAL_ORIGINAL = '''        size_t remain_size = stub->segments[seg].size;
        const uint8_t *data_pos = stub->segments[seg].data;
        while (remain_size > 0) {
            size_t data_size = MIN(ESP_RAM_BLOCK, remain_size);
            RETURN_ON_ERROR(esp_loader_mem_write(loader, &mem_cfg, data_pos, data_size));
            data_pos += data_size;
            remain_size -= data_size;
        }'''
SERIAL_PACKED = '''        size_t remain_size = stub->segments[seg].size;
        uint8_t *unpacked = solar_os_flash_stub_unpack(stub->segments[seg].data, remain_size);
        if (!unpacked) {
            return ESP_LOADER_ERROR_FAIL;
        }
        const uint8_t *data_pos = unpacked;
        esp_loader_error_t write_result = ESP_LOADER_SUCCESS;
        while (remain_size > 0 && write_result == ESP_LOADER_SUCCESS) {
            size_t data_size = MIN(ESP_RAM_BLOCK, remain_size);
            write_result = esp_loader_mem_write(loader, &mem_cfg, data_pos, data_size);
            data_pos += data_size;
            remain_size -= data_size;
        }
        solar_os_flash_stub_release(unpacked);
        RETURN_ON_ERROR(write_result);'''

SDIO_ORIGINAL = '''        RETURN_ON_ERROR(sip_upload_ram_segment(loader, stub->segments[seg].addr,
                                               stub->segments[seg].data,
                                               stub->segments[seg].size));'''
SDIO_PACKED = '''        const uint32_t size = stub->segments[seg].size;
        uint8_t *unpacked = size ? solar_os_flash_stub_unpack(stub->segments[seg].data, size) : NULL;
        if (size && !unpacked) {
            return ESP_LOADER_ERROR_FAIL;
        }
        const esp_loader_error_t write_result =
            sip_upload_ram_segment(loader, stub->segments[seg].addr, unpacked, size);
        solar_os_flash_stub_release(unpacked);
        RETURN_ON_ERROR(write_result);'''


def replace_once(source, original, replacement):
    if source.count(original) != 1:
        raise ValueError(f'unexpected serial flasher source near {original.splitlines()[0]}')
    return source.replace(original, replacement, 1)


def format_bytes(data):
    return '\n' + ''.join(
        '    ' + ', '.join(f'0x{byte:02x}' for byte in data[index:index + 16]) + ',\n'
        for index in range(0, len(data), 16))


def pack_stub(source):
    raw_sizes = {}

    def encode(match):
        data = match[3]
        if re.sub(r'[\s,]+', '', BYTE.sub('', data)):
            raise ValueError(f'unexpected initializer for serial stub {match[2]}')
        raw = bytes(int(byte, 16) for byte in BYTE.findall(data))
        if not raw or len(raw) % 4:
            raise ValueError(f'invalid word-aligned serial stub segment {match[2]}')
        packed = zlib.compress(raw, 9)
        if match[2] in raw_sizes or len(packed) > 65535:
            raise ValueError(f'invalid packed serial stub segment {match[2]}')
        raw_sizes[match[2]] = len(raw)
        return match[1] + format_bytes(struct.pack('<H', len(packed)) + packed) + match[4]

    source = ARRAY.sub(encode, source)
    if not raw_sizes:
        raise ValueError('serial stub contains no segments')
    for name, raw_size in raw_sizes.items():
        size = re.compile(r'(\.size\s*=\s*)(sizeof\(' + name + r'\)|\d+)'
                          r'(,\s*\.data\s*=\s*' + name + r'\s*,)')
        matches = list(size.finditer(source))
        if len(matches) != 1 or matches[0][2] not in (f'sizeof({name})', str(raw_size)):
            raise ValueError(f'unexpected size for serial stub segment {name}')
        source = size.sub(lambda match: f'{match[1]}{raw_size}{match[3]}', source)
    return source


def write_changed(path, content):
    if not path.exists() or path.read_text() != content:
        path.write_text(content)


def pack_component(component, output):
    source_dir = component / 'src'
    stubs = sorted((source_dir / 'stubs').glob('esp_stub_*.c'))
    if not stubs:
        raise ValueError('serial flasher has no stub sources')
    output.mkdir(parents=True, exist_ok=True)
    for path in stubs:
        write_changed(output / path.name, pack_stub(path.read_text()))
    for name, old, new in (('esp_loader.c', SERIAL_ORIGINAL, SERIAL_PACKED),
                           ('protocol_sdio.c', SDIO_ORIGINAL, SDIO_PACKED)):
        source = replace_once((source_dir / name).read_text(), old, new)
        source = replace_once(source, INCLUDE, INCLUDE + '\n#include "solar_os_flash_stub.h"')
        if re.search(r'->segments\[[^]]+\]\.data', source.replace(new, '')):
            raise ValueError(f'unreviewed stub transfer in {name}')
        write_changed(output / name, source)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('component', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    pack_component(args.component, args.output)


if __name__ == '__main__':
    main()
