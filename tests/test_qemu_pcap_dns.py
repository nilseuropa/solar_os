"""Packet lines of scripts/qemu_pcap_dns.py."""
import importlib.util
from pathlib import Path
import struct
import sys
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("qemu_pcap_dns", REPOSITORY / "scripts" / "qemu_pcap_dns.py")
assert SPEC is not None and SPEC.loader is not None
qemu_pcap_dns = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = qemu_pcap_dns
SPEC.loader.exec_module(qemu_pcap_dns)

QNAME = b"\x04pool\x03ntp\x03org\x00"
GUEST = bytes([10, 0, 2, 15])
DNS = bytes([10, 0, 2, 3])


def frame(src: bytes, dst: bytes, sport: int, dport: int, payload: bytes) -> bytes:
    udp = struct.pack("!HHHH", sport, dport, 8 + len(payload), 0) + payload
    iph = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0, src, dst)
    return b"\x00" * 12 + b"\x08\x00" + iph + udp


def pcap(frames: list[tuple[float, bytes]]) -> bytes:
    out = struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1)
    for t, f in frames:
        out += struct.pack("<IIII", 1000 + int(t), int((t % 1) * 1e6), len(f), len(f)) + f
    return out


class PcapDnsTests(unittest.TestCase):
    def test_query_and_answer_with_compressed_name(self) -> None:
        query = struct.pack("!HHHHHH", 0x1234, 0x0100, 1, 0, 0, 0) + QNAME + struct.pack("!HH", 1, 1)
        answer = (struct.pack("!HHHHHH", 0x1234, 0x8180, 1, 1, 0, 0) + QNAME + struct.pack("!HH", 1, 1)
                  + b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 60, 4) + bytes([1, 2, 3, 4]))
        got = qemu_pcap_dns.lines(pcap([(0.0, frame(GUEST, DNS, 4000, 53, query)),
                                        (2.5, frame(DNS, GUEST, 53, 4000, answer))]))
        self.assertEqual(got, [
            "   0.000 s  10.0.2.15:4000 -> 10.0.2.3:53 DNS id=1234 query pool.ntp.org A",
            "   2.500 s  10.0.2.3:53 -> 10.0.2.15:4000 DNS id=1234 answer pool.ntp.org A rcode=ok -> 1.2.3.4",
        ])

    def test_servfail_and_ntp(self) -> None:
        fail = struct.pack("!HHHHHH", 7, 0x8182, 1, 0, 0, 0) + QNAME + struct.pack("!HH", 1, 1)
        got = qemu_pcap_dns.lines(pcap([(0.0, frame(DNS, GUEST, 53, 4000, fail)),
                                        (1.0, frame(GUEST, bytes([1, 2, 3, 4]), 123, 123, b"\x00" * 48))]))
        self.assertTrue(got[0].endswith("rcode=SERVFAIL -> -"))
        self.assertTrue(got[1].endswith("10.0.2.15:123 -> 1.2.3.4:123 NTP request"))


if __name__ == "__main__":
    unittest.main()
