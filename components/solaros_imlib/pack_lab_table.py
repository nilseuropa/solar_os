#!/usr/bin/env python3
"""Keep the OpenMV RGB565 conversion table lossless with fewer flash bytes."""

import argparse
from collections import defaultdict, deque
from pathlib import Path
import re


def pack_lightness(lightness):
    row_bytes = 8
    rows = [bytes(lightness[i:i + row_bytes]) for i in range(0, len(lightness), row_bytes)]
    unique_rows = dict.fromkeys(rows)
    prefixes = defaultdict(deque)
    for row in unique_rows:
        for length in range(1, row_bytes):
            prefixes[row[:length]].append(row)

    # Share suffixes and prefixes in the byte table. The original offset plus
    # pixel position still reads a contiguous row with the same two loads.
    packed = bytearray()
    offsets = {}
    for first in unique_rows:
        if first in offsets:
            continue
        row, overlap = first, 0
        while row is not None:
            offsets[row] = len(packed) - overlap
            packed.extend(row[overlap:])
            row = None
            for overlap in range(row_bytes - 1, 0, -1):
                candidates = prefixes.get(bytes(packed[-overlap:]))
                while candidates and candidates[0] in offsets:
                    candidates.popleft()
                if candidates:
                    row = candidates.popleft()
                    break

    return [offsets[row] for row in rows], packed


def pack(source):
    copyright_notice, _, definition = source.partition('#include <stdint.h>')
    initializer = re.fullmatch(r'\s*const int8_t lab_table\[98304\] = \{([\d\s,\-]+)\};\s*', definition)
    if not initializer:
        raise ValueError('unexpected shape of the OpenMV LAB table')
    channels = list(map(int, re.findall(r'-?\d+', initializer[1])))
    if len(channels) != 3 * 32768 or any(value < -128 or value > 127 for value in channels):
        raise ValueError('invalid OpenMV LAB values')

    yield copyright_notice.rstrip()
    yield '#include "solaros_imlib_lab.h"'
    yield ''
    yield 'const solaros_imlib_lab_ab_t solaros_imlib_lab_ab[32768] = {'
    for start in range(0, len(channels), 24):
        yield '    ' + ''.join(f'{{{channels[i + 1]},{channels[i + 2]}}},' for i in range(start, start + 24, 3))
    yield '};'

    lightness = channels[::3]
    if any(value < 0 or value > 100 for value in lightness):
        raise ValueError('invalid OpenMV LAB lightness')
    offsets, packed = pack_lightness(lightness)

    yield ''
    yield 'const uint16_t solaros_imlib_lab_l_offsets[4096] = {'
    for index in range(0, len(offsets), 16):
        yield '    ' + ','.join(str(value) for value in offsets[index:index + 16]) + ','
    yield '};'
    yield ''
    yield 'const uint8_t solaros_imlib_lab_l_rows[] = {'
    for index in range(0, len(packed), 16):
        yield '    ' + ','.join(str(value) for value in packed[index:index + 16]) + ','
    yield '};'
    yield '_Static_assert(sizeof(solaros_imlib_lab_ab) + sizeof(solaros_imlib_lab_l_offsets) +'
    yield '               sizeof(solaros_imlib_lab_l_rows) < 98304, "packed LAB table saves flash");'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    text = '\n'.join(pack(args.input.read_text())) + '\n'
    if not args.output.exists() or args.output.read_text() != text:
        args.output.write_text(text)


if __name__ == '__main__':
    main()
