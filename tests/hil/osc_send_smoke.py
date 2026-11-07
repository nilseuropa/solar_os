#!/usr/bin/env python3
"""Test OSC CLI output over UDP and optional Solbot loopback using zero velocities.

Requires an updated device and exclusive access to its Telnet shell. --robot
loads solbot.urdf if necessary, starts its controllers, requires no existing
OSC bindings/job, and leaves the robot waiting with zero output after success.
"""
import argparse
import json
from pathlib import Path
import re
import socket
import struct
import threading
import time


def command(host, text, timeout=10, interrupt=None, disconnect=False):
    with socket.create_connection((host, 23), timeout=3) as connection:
        connection.settimeout(.1)
        initial = bytearray()
        end = time.monotonic() + .3
        while time.monotonic() < end:
            try:
                initial.extend(connection.recv(8192))
            except socket.timeout:
                pass
        if b"Telnet is busy" in initial:
            raise RuntimeError("Device Telnet shell is busy")
        connection.sendall(b"\xff\xfb\x1f\xff\xfa\x1f\x00\xf0\x00\x3c\xff\xf0")
        time.sleep(.1)
        connection.sendall((text + "\r\n").encode())
        started = time.monotonic()
        data = bytearray(initial)
        clean = ""
        while time.monotonic() - started < timeout:
            if interrupt is not None and time.monotonic() - started >= interrupt:
                if disconnect:
                    break
                connection.sendall(b"\x03")
                interrupt = None
            try:
                block = connection.recv(32768)
                if not block:
                    break
                data.extend(block)
                clean = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", data.decode(errors="replace"))
                if text in clean and re.search(r"(?:^|\n)[^ \r\n]+@[^ \r\n]+:[^\r\n]* $", clean):
                    break
            except socket.timeout:
                pass
    time.sleep(.3)
    if not disconnect and (text not in clean or not re.search(r"(?:^|\n)[^ \r\n]+@[^ \r\n]+:[^\r\n]* $", clean)):
        raise RuntimeError("Command did not finish: " + text + "\n" + clean)
    return clean


def decode(packet):
    end = packet.index(0)
    address = packet[:end].decode("ascii")
    offset = (end + 4) & ~3
    end = packet.index(0, offset)
    types = packet[offset:end].decode("ascii")
    assert types.startswith(",") and set(types[1:]) == {"f"}, types
    offset = (end + 4) & ~3
    count = len(types) - 1
    assert len(packet) == offset + 4 * count
    return address, struct.unpack(">" + "f" * count, packet[offset:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.1.238")
    parser.add_argument("--robot", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = {"host": args.host, "checks": []}

    def run(text, **options):
        output = command(args.host, text, **options)
        print(output, flush=True)
        return output

    route = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    route.connect((args.host, 23))
    local_ip = route.getsockname()[0]
    device_ip = socket.gethostbyname(args.host)
    route.close()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        listener.bind((local_ip, 0))
        listener.settimeout(.1)
        target = "%s:%d" % listener.getsockname()

        def capture(text, **options):
            received = []
            stop = threading.Event()

            def collect():
                while not stop.is_set():
                    try:
                        data, peer = listener.recvfrom(2048)
                        if peer[0] == device_ip:
                            received.append((time.monotonic(), data))
                    except socket.timeout:
                        pass

            thread = threading.Thread(target=collect)
            thread.start()
            try:
                output = run(text, **options)
                ended = time.monotonic()
                time.sleep(.3)
            finally:
                stop.set()
                thread.join(2)
            assert not thread.is_alive()
            assert all(t <= ended + .05 for t, _ in received), "Sender continued after command exit"
            return output, received

        out, packets = capture("osc send %s /base/cmd_vel 0 0" % target)
        assert "sent 1 message" in out and len(packets) == 1
        assert decode(packets[0][1]) == ("/base/cmd_vel", (0., 0.))
        report["checks"].append("one message with two float32 arguments, no OSC job required")
        out, packets = capture("osc send %s /values 1 -0.5 0 2 3 4 5 6" % target)
        assert len(packets) == 1 and decode(packets[0][1]) == ("/values", (1., -.5, 0., 2., 3., 4., 5., 6.))
        report["checks"].append("eight float32 arguments in one packet")
        out, packets = capture("osc send %s /base/cmd_vel 0 0 --rate 20 --duration 1" % target)
        assert 18 <= len(packets) <= 21, len(packets)
        assert all(decode(data) == ("/base/cmd_vel", (0., 0.)) for _, data in packets)
        report["periodic_packets"] = len(packets)
        report["periodic_span_seconds"] = packets[-1][0] - packets[0][0]
        assert .8 <= report["periodic_span_seconds"] <= 1.1
        report["checks"].append("20 Hz timed publication and automatic stop")
        for disconnect in (False, True):
            out, packets = capture("osc send %s /base/cmd_vel 0 0 --rate 20" % target,
                                   interrupt=.35, disconnect=disconnect)
            assert 2 <= len(packets) <= 12, len(packets)
            if not disconnect:
                assert "cancelled" in out
            report["checks"].append("disconnect stops transmission" if disconnect else "Ctrl+C stops transmission")
        for suffix, error in (("nan", "invalid value"), ("0 --duration 1", "requires --rate"),
                              ("0 --rate 101", "invalid rate")):
            out, packets = capture("osc send %s /base/cmd_vel %s" % (target, suffix))
            assert error in out and not packets, out
        report["checks"].append("invalid commands emit no packets")
        # Managed UDP has eight global slots: leaked sessions fail this loop.
        for _ in range(12):
            out, packets = capture("osc send %s /base/cmd_vel 0 0" % target)
            assert "sent 1 message" in out and len(packets) == 1
        report["checks"].append("repeated invocations release managed UDP channels")

    if args.robot:
        assert "No OSC bindings" in run("osc bindings"), "Preserve existing OSC configuration before this test"
        assert "stopped" in run("job status osc"), "Stop the existing OSC job before this test"
        if "solbot:" not in run("robot model info"):
            assert "Robot model loaded" in run("robot model load solbot.urdf")
        if "No joints" in run("robot joint list"):
            assert "controllers started" in run("robot start")
        bindings = []
        running = passed = False
        try:
            for name, axis, index in (("cli-linear", "linear", 0), ("cli-angular", "angular", 1)):
                assert "created" in run("osc bind %s receive diff_drive.base.cmd_%s /test/base_vel arg=%d" % (name, axis, index))
                bindings.append(name)
            assert "job started" in run("job start osc listen=19000")
            running = True
            out = run("osc send 127.0.0.1:19000 /test/base_vel 0 0 --rate 20 --duration 1")
            sent = int(re.search(r"OSC sent (\d+) messages", out).group(1))
            state = run("osc bindings")
            counts = [int(n) for n in re.findall(r"received=(\d+)", state)]
            assert counts == [sent, sent] and state.count("write-errors=0") == 2, state
            assert "known=4" in run("robot model info")
            joints = run("robot joint list")
            assert joints.count("velocity waiting") == 2 and joints.count("faults=0") == 2 and joints.count("output=0 ") == 2, joints
            report["checks"].append("loopback paired base commands reach both streams; watchdog returns to waiting with zero output")
            passed = True
        finally:
            if running:
                run("job stop osc")
            for name in bindings:
                run("osc unbind " + name)
            if not passed:
                run("robot stop")
        assert "No OSC bindings" in run("osc bindings")
        assert "stopped" in run("job status osc")
    report["passed"] = True
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print("OSC_SEND_HIL_PASS", json.dumps(report), flush=True)


if __name__ == "__main__":
    main()
