#!/usr/bin/env python3
"""Opt-in IRC HIL against a USB shell and two local, verified TLS servers.

Requires pyserial and cryptography. Pass --host-address with the LAN address
reachable by the device. Creates only hil-irc-a/b profiles and removes them
on success. The caller starts the USB port shell before running this test.
--test-removal also exercises display-shell, Chat, and Python instance removal;
--keep-removal-records leaves its removal markers for a reboot recovery check.
"""
import argparse
import base64
import datetime
import ipaddress
import json
import re
import socket
import ssl
import tempfile
import threading
import time
from pathlib import Path

import serial
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID


ANSI = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))")
PROMPT = re.compile(r"[\w.-]+@[\w.-]+:[^\r\n]* ")
HIL_PASSWORD = "hil password\\with'and\"quotes"


class Console:
    def __init__(self, port):
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=0.1)
        self.serial.dtr = True
        self.serial.rts = False
        self.serial.port = port
        self.serial.open()

    def collect(self, duration=0.6):
        end = time.monotonic() + duration
        output = bytearray()
        while time.monotonic() < end:
            output.extend(self.serial.read(self.serial.in_waiting or 1))
        return ANSI.sub("", output.decode(errors="replace")).replace("\r", "")

    def command(self, command, timeout=15):
        self.serial.reset_input_buffer()
        self.serial.write(command.encode() + b"\r")
        output = ""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            output += self.collect(0.15)
            if "\n" in output and PROMPT.search(output):
                print(output, flush=True)
                return output
        raise TimeoutError(f"shell command {command.split()[0] if command else '(prompt)'}: {output}")

    def wait_for(self, marker, timeout=20):
        output = ""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            output += self.collect(0.2)
            if marker in output:
                return output
        raise TimeoutError(f"console marker {marker!r}: {output}")


class IRCServer:
    def __init__(self, context, label, sasl=False):
        self.context, self.label, self.sasl = context, label, sasl
        self.listener = socket.socket()
        self.listener.bind(("0.0.0.0", 0))
        self.listener.listen()
        self.port = self.listener.getsockname()[1]
        self.events, self.clients, self.errors = [], [], []
        self.connections = 0
        self.rejected_tls = 0
        self.rejected_auth = 0
        threading.Thread(target=self.accept, daemon=True).start()

    def accept(self):
        while True:
            client, _ = self.listener.accept()
            threading.Thread(target=self.session, args=(client,), daemon=True).start()

    def session(self, raw):
        try:
            client = self.context.wrap_socket(raw, server_side=True)
            self.clients.append(client)
            self.connections += 1
            nickname, user, authenticated, welcomed = "*", False, not self.sasl, False

            def send(line):
                client.sendall((line + "\r\n").encode())

            reader = client.makefile("rb")
            while line := reader.readline():
                text = line.decode().rstrip("\r\n")
                self.events.append((time.monotonic(), text))
                command, _, rest = text.partition(" ")
                if command == "CAP" and rest == "LS 302":
                    send(f":test CAP {nickname} LS * :multi-prefix")
                    send(f":test CAP {nickname} LS :sasl=PLAIN")
                elif command == "CAP" and rest == "REQ :sasl":
                    send(f":test CAP {nickname} ACK :sasl")
                elif text == "AUTHENTICATE PLAIN":
                    send("AUTHENTICATE +")
                elif command == "AUTHENTICATE":
                    if base64.b64decode(rest) == b"\0hil-account\0" + HIL_PASSWORD.encode():
                        authenticated = True
                        send(f":test 903 {nickname} :SASL authentication successful")
                    else:
                        self.rejected_auth += 1
                        send(f":test 904 {nickname} :SASL authentication failed: Invalid credentials")
                elif command == "NICK":
                    nickname = rest
                elif command == "USER":
                    user = True
                elif command == "JOIN":
                    channel = rest.split()[0]
                    send(f":{nickname}!self@test JOIN :{channel}")
                    # Exercise split reads and interleaved keepalive traffic.
                    packet = f":Remote!u@host PRIVMSG {channel} :hello-{self.label}\r\n".encode()
                    client.sendall(packet[:17])
                    time.sleep(0.03)
                    client.sendall(packet[17:])
                    client.sendall(packet)  # Identical live lines must remain distinct.
                    send(f":Remote!u@host PRIVMSG {nickname} :private-{self.label}")
                    send("PING :hil-keepalive")
                elif command == "PART":
                    send(f":{nickname}!self@test PART {rest}")
                elif command == "PING":
                    send("PONG :solaros")
                if user and nickname != "*" and authenticated and not welcomed and (not self.sasl or text == "CAP END"):
                    send(f":test 001 {nickname} :Welcome")
                    send(f":test 005 {nickname} CASEMAPPING=rfc1459 CHANTYPES=#& :supported")
                    welcomed = True
        except (ssl.SSLError, OSError):
            self.rejected_tls += 1
        except Exception as error:
            self.errors.append(str(error))

    def wait(self, predicate, timeout=35):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.errors:
                raise AssertionError(self.errors)
            if predicate():
                return
            time.sleep(0.1)
        raise TimeoutError(f"{self.label}: {self.events}")

    def drop(self):
        for client in self.clients:
            try:
                client.shutdown(socket.SHUT_RDWR)
                client.close()
            except OSError:
                pass
        self.clients.clear()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--host-address", required=True)
    parser.add_argument("--test-removal", action="store_true")
    parser.add_argument("--display-session", type=int, default=0)
    parser.add_argument("--keep-removal-records", action="store_true")
    args = parser.parse_args()
    assert not args.keep_removal_records or args.test_removal
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "SolarOS IRC HIL")])
    certificate = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
        .public_key(key.public_key()).serial_number(x509.random_serial_number())
        .not_valid_before(datetime.datetime(2020, 1, 1))
        .not_valid_after(datetime.datetime(2040, 1, 1))
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address(args.host_address))]), critical=False)
        .sign(key, hashes.SHA256()))
    with tempfile.TemporaryDirectory(prefix="solaros-irc-hil-") as directory:
        pem = certificate.public_bytes(serialization.Encoding.PEM)
        cert_file, key_file = Path(directory) / "ca.pem", Path(directory) / "key.pem"
        cert_file.write_bytes(pem)
        key_file.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert_file, key_file)
        a, b = IRCServer(context, "a"), IRCServer(context, "b", sasl=True)
        console = Console(args.port)
        console.command("")
        existing = console.command("irc status")
        assert "hil-irc-a" not in existing and "hil-irc-b" not in existing, "reserved HIL names already exist"
        before = console.command("mem policy")
        nvs_before = console.command("nvs status")
        console.serial.write(b"python\r")
        console.wait_for(">>>")
        device_ca = "/sdcard/irc-hil-ca.pem"
        def repl(line):
            # The REPL limits input to the terminal's visible columns.
            assert len(line.encode()) < 72
            encoded = line.encode() + b"\r"
            for offset in range(0, len(encoded), 12):
                console.serial.write(encoded[offset:offset + 12])
                time.sleep(0.04)
            output = ""
            deadline = time.monotonic() + 12
            while ">>>" not in output and time.monotonic() < deadline:
                output += console.collect(0.2)
            assert ">>>" in output and "Error" not in output and "Traceback" not in output, output
            return output

        repl(f"f=open({device_ca!r},'w')")
        text = pem.decode()
        for offset in range(0, len(text), 40):
            repl(f"f.write({text[offset:offset + 40]!r})")
        repl("f.close()")
        console.serial.write(b"exit()\r")
        assert PROMPT.search(console.collect(2))
        ids = []
        invalid = console.command(f"messages add irc hil-invalid {args.host_address} {a.port} nils@sol")
        assert "saved instance" not in invalid and "hil-invalid:" not in console.command("irc status"), invalid
        for label, server in [("a", a), ("b", b)]:
            setup = "messages add irc" if label == "a" else "irc add"
            output = console.command(f"{setup} hil-irc-{label} {args.host_address} {server.port} pocket{label}")
            ids.append(int(re.search(r"saved instance .* \((\d+)\)", output)[1]))
            if label == "b":
                assert "irc: ok" in console.command(f"irc ca hil-irc-{label} {device_ca}")
            if server.sasl:
                assert "configuration saved" in console.command(f"messages configure hil-irc-{label} sasl hil-account hil-wrong-password")
            assert "irc: ok" in console.command(f"irc connect hil-irc-{label}")
        console.command("job start irc-sync")
        # A must reject its untrusted certificate. B must expose an exact SASL
        # rejection without registering, then recover after credentials change.
        a.wait(lambda: a.rejected_tls > 0)
        assert a.connections == 0 and not a.events, "untrusted TLS connection was accepted"
        b.wait(lambda: b.rejected_auth > 0)
        deadline = time.monotonic() + 15
        while True:
            status = console.command("irc status")
            if "IRC SASL 904: SASL authentication failed: Invalid credentials" in status:
                break
            assert time.monotonic() < deadline, status
        assert not any(event[1] == "CAP END" for event in b.events), "rejected SASL fell back to anonymous login"
        # Exercise Chat's actual setup parser with spaces, backslashes and
        # both kinds of quote in the password. The server checks exact bytes.
        console.command("setterm profile ansi")
        output = console.command("messages join hil-irc-b #same")
        failed_room = int(re.search(r"conversation (\d+)", output)[1])
        console.serial.write(f"chat {failed_room}\r".encode())
        view = console.wait_for("Channels")
        if "Invalid credentials" not in view:
            view += console.wait_for("Invalid credentials")
        assert "error" in view, view
        console.serial.write(("/sasl hil-irc-b hil-account " + json.dumps(HIL_PASSWORD) + "\r").encode())
        b.wait(lambda: any(event[1] == "CAP END" for event in b.events))
        console.collect(1)
        console.serial.write(b"\x1d")
        assert PROMPT.search(console.collect(2)), "Chat setup did not restore the shell prompt"
        assert "irc: ok" in console.command(f"irc ca hil-irc-a {device_ca}")
        for server in (a, b):
            server.wait(lambda s=server: any("USER " in event[1] for event in s.events))
        for label in ("a", "b"):
            assert "conversation" in console.command(f"messages join hil-irc-{label} #same")
        for server in (a, b):
            server.wait(lambda s=server: any(event[1] == "PONG :hil-keepalive" for event in s.events))
        output = console.command("messages conversations")
        rooms = {}
        for line in output.splitlines():
            match = re.match(r"(\d+)\s+hil-irc-([ab])\s+room\b.*#same", line)
            if match:
                rooms[match[2]] = int(match[1])
        assert len(rooms) == 2 and rooms["a"] != rooms["b"], output
        for label, room in rooms.items():
            output = console.command(f"messages list {room}")
            assert output.count(f"hello-{label}") == 2 and f"hello-{'b' if label == 'a' else 'a'}" not in output
            output = console.command(f"messages query hil-irc-{label} Remote")
            direct = int(re.search(r"conversation (\d+)", output)[1])
            assert f"private-{label}" in console.command(f"messages list {direct}")
            console.command(f"messages send {direct} direct-{label}")
        for label, server in (("a", a), ("b", b)):
            server.wait(lambda s=server, label=label: any(event[1] == f"PRIVMSG remote :direct-{label}" for event in s.events))
        for i in range(3):
            console.command(f"messages send {rooms['a']} flood-{i}")
        a.wait(lambda: len([event for event in a.events if event[1].startswith("PRIVMSG #same :flood-")]) == 3)
        sends = [event[0] for event in a.events if event[1].startswith("PRIVMSG #same :flood-")]
        assert all(y - x >= 0.9 for x, y in zip(sends, sends[1:])), sends
        old_a, old_b = a.connections, b.connections
        a.drop()
        a.wait(lambda: a.connections > old_a and sum(event[1] == "JOIN #same" for event in a.events) >= 2)
        assert b.connections == old_b, "reconnecting A disturbed B"
        console.command("setterm profile ansi")
        console.serial.write(b"chat\r")
        view = console.wait_for("Channels")
        assert "channels" in view.lower(), view
        console.serial.write(b"\t/join hil-irc-a #ui-check\r")
        a.wait(lambda: any(event[1] == "JOIN #ui-check" for event in a.events))
        view = console.collect(2)
        assert "hello-a" in view or "joined" in view, view
        console.serial.write(b"/query hil-irc-b Remote\r")
        view = console.collect(2)
        assert "private-b" in view, view
        console.serial.write(b"\x1d")
        assert PROMPT.search(console.collect(2)), "Chat did not restore the shell prompt"
        for server in (a, b):
            assert not any("history" in event[1] or "echo-message" in event[1] for event in server.events)
        console.command("irc status")
        after = console.command("mem policy")
        nvs_after = console.command("nvs status")
        removal = None
        if args.test_removal:
            for i in range(8):
                assert "messages: queued" in console.command(f"messages send {rooms['a']} remove-queued-{i}")
            before_remove = console.command("mem policy")
            assert "sent command" in console.command(f"session send {args.display_session} irc remove hil-irc-a")
            console.collect(1)
            status = console.command("irc status")
            assert "hil-irc-a:" not in status and "hil-irc-b: connected" in status, status
            history = console.command(f"messages list {rooms['a']}")
            assert "hello-a" not in history and "remove-queued-" not in history, history
            assert "messages: queued" not in console.command(f"messages send {rooms['a']} must-fail")
            connections_after_remove = a.connections
            b.clients[-1].sendall(b"PING :after-remove\r\n")
            b.wait(lambda: any(event[1] == "PONG :after-remove" for event in b.events))
            console.collect(2)
            assert a.connections == connections_after_remove and b.connections == old_b
            after_remove = console.command("mem policy")
            output = console.command(f"irc add hil-irc-a {args.host_address} {a.port} pocketnew")
            replacement = int(re.search(r"saved instance .* \((\d+)\)", output)[1])
            assert replacement > max(ids), output
            console.serial.write(b"chat\r")
            console.wait_for("Channels")
            console.serial.write(b"\t/remove hil-irc-a\r")
            # Incremental terminal redraws may split the status text across
            # cursor movements; verify removal through the service below.
            console.wait_for("removed")
            console.serial.write(b"\x1d")
            assert PROMPT.search(console.collect(1))
            assert "hil-irc-a:" not in console.command("irc status")
            removal = {"retired": ids[0], "replacement": replacement,
                "memory_before": before_remove, "memory_after": after_remove,
                "tasks": console.command("top")}
        console.command("job stop irc-sync")
        offline = 'b' if args.test_removal else 'a'
        console.serial.write(f"chat {rooms[offline]}\r".encode())
        console.wait_for("Channels")
        # A conversation-ID selector starts directly on the Chat tab. Go to
        # Channels and select it again to exercise offline membership handling.
        console.serial.write(b"\t")
        console.collect(0.6)  # Commit the intermediate frame before selecting.
        console.serial.write(b"\r")
        view = console.collect(2)
        assert f"hello-{offline}" in view, "retained IRC history was not readable with the job stopped"
        console.serial.write(b"\x1d")
        assert PROMPT.search(console.collect(1))
        console.serial.write(b"python\r")
        console.wait_for(">>>")
        repl("import solaros")
        assert "hil-irc-b" in repl("print(solaros.messages.instances()[-1]['name'])")
        repl(f"print(solaros.messages.query({ids[1]},'Remote'))")
        if args.test_removal:
            repl(f"solaros.messages.remove_instance({ids[1]})")
            assert "hil-irc-" not in repl("print(solaros.messages.instances())")
            repl(f"p=open('/sdcard/.chat/profiles/{ids[0]:03}.bin','rb').read()")
            assert "True" in repl("print(len(p)<128 and b'hil-password' not in p)")
        console.serial.write(b"exit()\r")
        assert PROMPT.search(console.collect(2))
        for instance in ids + ([removal["replacement"]] if removal else []):
            assert "instance and history removed" in console.command(f"messages remove {instance}")
            if not args.keep_removal_records:
                console.command(f"messages clear {instance}")
                console.command(f"rm /sdcard/.chat/profiles/{instance:03}.bin")
        console.command(f"rm {device_ca}")
        print(json.dumps({"result": "pass", "instances": ids, "flood_spacing_seconds": [round(y - x, 3) for x, y in zip(sends, sends[1:])],
            "connections": {"a": a.connections, "b": b.connections}, "sasl_rejections": b.rejected_auth,
            "memory_before": before, "memory_after": after,
            "nvs_before": nvs_before, "nvs_after": nvs_after, "removal": removal}, indent=2))
        console.serial.close()


if __name__ == "__main__":
    main()
