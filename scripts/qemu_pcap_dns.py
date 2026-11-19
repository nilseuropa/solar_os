#!/usr/bin/env python3
"""qemu_pcap_dns.py FILE: print the DHCP, ARP, DNS and NTP packets of a QEMU filter-dump pcap, one line each.

For debugging the network of the qemu_esp32s3_rgb board. Record with
    -netdev user,id=n0 -net nic,model=open_eth,netdev=n0 -object filter-dump,id=f0,netdev=n0,file=net.pcap
in place of -nic user,model=open_eth; see doc/manual/qemu.md.
"""
import struct
import sys

RCODES = {0: "ok", 1: "FORMERR", 2: "SERVFAIL", 3: "NXDOMAIN", 4: "NOTIMP", 5: "REFUSED"}
QTYPES = {1: "A", 28: "AAAA", 5: "CNAME"}


def ip(b):
    return ".".join(str(x) for x in b)


def dns_name(msg, off):
    """Return (name, offset after the name in the original place)."""
    labels, end, jumps = [], None, 0
    while True:
        n = msg[off]
        if n & 0xC0 == 0xC0:
            if end is None:
                end = off + 2
            off = ((n & 0x3F) << 8) | msg[off + 1]
            jumps += 1
            if jumps > 20:
                raise ValueError("pointer loop")
            continue
        off += 1
        if n == 0:
            break
        labels.append(msg[off:off + n].decode("ascii", "replace"))
        off += n
    return ".".join(labels), end if end is not None else off


def dns_text(msg):
    qid, flags, qd, an = struct.unpack("!HHHH", msg[:8])
    off = 12
    names = []
    for _ in range(qd):
        name, off = dns_name(msg, off)
        qtype = struct.unpack("!H", msg[off:off + 2])[0]
        names.append(f"{name} {QTYPES.get(qtype, qtype)}")
        off += 4
    text = f"DNS id={qid:04x} {'answer' if flags & 0x8000 else 'query'} {', '.join(names)}"
    if not flags & 0x8000:
        return text
    answers = []
    for _ in range(an):
        _, off = dns_name(msg, off)
        rtype, _, _, rdlen = struct.unpack("!HHIH", msg[off:off + 10])
        off += 10
        if rtype == 1 and rdlen == 4:
            answers.append(ip(msg[off:off + 4]))
        else:
            answers.append(QTYPES.get(rtype, str(rtype)))
        off += rdlen
    return f"{text} rcode={RCODES.get(flags & 0xF, flags & 0xF)} -> {', '.join(answers) or '-'}"


def packet_text(frame):
    etype = struct.unpack("!H", frame[12:14])[0]
    if etype == 0x0806:
        op = struct.unpack("!H", frame[20:22])[0]
        return f"ARP {'who-has' if op == 1 else 'reply'} {ip(frame[38:42])} from {ip(frame[28:32])}"
    if etype != 0x0800:
        return None
    ihl = (frame[14] & 0x0F) * 4
    if frame[23] != 17:
        return None
    src, dst = ip(frame[26:30]), ip(frame[30:34])
    udp = frame[14 + ihl:]
    sport, dport = struct.unpack("!HH", udp[:4])
    payload = udp[8:]
    arrow = f"{src}:{sport} -> {dst}:{dport}"
    if 53 in (sport, dport):
        return f"{arrow} {dns_text(payload)}"
    if 123 in (sport, dport):
        return f"{arrow} NTP {'request' if dport == 123 else 'reply'}"
    if {sport, dport} & {67, 68}:
        return f"{arrow} DHCP"
    return None


def lines(data):
    magic = struct.unpack("<I", data[:4])[0]
    if magic != 0xA1B2C3D4:
        raise ValueError(f"not a little-endian microsecond pcap: magic {magic:08x}")
    off, t0, out = 24, None, []
    while off + 16 <= len(data):
        sec, usec, incl, _ = struct.unpack("<IIII", data[off:off + 16])
        frame = data[off + 16:off + 16 + incl]
        off += 16 + incl
        t = sec + usec / 1e6
        t0 = t if t0 is None else t0
        try:
            text = packet_text(frame)
        except (IndexError, ValueError, struct.error) as exc:
            text = f"unreadable packet ({exc})"
        if text:
            out.append(f"{t - t0:8.3f} s  {text}")
    return out


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as f:
        print("\n".join(lines(f.read())) or "no DHCP, ARP, DNS or NTP packets")
