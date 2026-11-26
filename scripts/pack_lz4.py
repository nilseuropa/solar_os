"""Encode bounded independent LZ4 blocks for built-in assets."""


def append_length(out, length):
    whole, remain = divmod(length, 255)
    out.extend(b'\xff' * whole)
    out.append(remain)


def append_sequence(out, literals, distance=0, length=4):
    out.append(min(len(literals), 15) << 4 | min(length - 4, 15))
    if len(literals) >= 15:
        append_length(out, len(literals) - 15)
    out.extend(literals)
    if distance:
        out.extend(distance.to_bytes(2, 'little'))
        if length >= 19:
            append_length(out, length - 19)


def compress_block(data):
    """Choose the longest available match in one block, with no dependencies."""
    out = bytearray()
    start = offset = 0
    while offset <= len(data) - 12:
        prefix = data[offset:offset + 4]
        candidate = data.rfind(prefix, 0, offset + 3)
        best, distance = 3, 0
        maximum = len(data) - offset - 5
        while candidate >= 0:
            size = 4
            while size < maximum and data[candidate + size] == data[offset + size]:
                size += 1
            if size > best:
                best, distance = size, offset - candidate
                if best == maximum:
                    break
            candidate = data.rfind(prefix, 0, candidate + 3)
        if not distance:
            offset += 1
            continue
        append_sequence(out, data[start:offset], distance, best)
        offset += best
        start = offset
    append_sequence(out, data[start:])
    return bytes(out) if len(out) < len(data) else data
