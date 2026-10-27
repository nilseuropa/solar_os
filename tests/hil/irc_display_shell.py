#!/usr/bin/env python3
"""Opt-in display-shell IRC stack and completion regression over a USB shell.

Run after flashing and starting the USB shell. Requires the selected display
shell to be at an empty prompt. Uses a disabled, temporary loopback profile
and removes it through the display shell after testing.
"""
import argparse
import json
import re

from irc_sync import Console


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--display-session", type=int, default=0)
    args = parser.parse_args()
    console = Console(args.port)
    name = "hil-irc-display"

    def display(command):
        output = console.command(f"session send {args.display_session} {command}")
        assert "sent command to session" in output, output
        console.collect(0.6)

    def status():
        output = console.command("irc status")
        match = re.search(rf"^(\d+) {name}:.*\n\s+([^\n]+)", output, re.M)
        assert match, output
        return int(match[1]), match[2]

    try:
        console.command("")
        # Exercise initialization on main before a USB command initializes IRC.
        display("irc status")
        assert name not in console.command("irc status"), "reserved HIL name already exists"
        display(f"irc add {name} 127.0.0.1 1 pocket")
        instance, settings = status()
        assert "enabled=no" in settings, settings

        # The display task must remain responsive through repeated profile I/O.
        for _ in range(3):
            display(f"irc connect {name}")
            assert "enabled=yes" in status()[1]
            display(f"irc disconnect {name}")
            assert "enabled=no" in status()[1]
        display(f"irc set {name} 127.0.0.1 1 pocket2")
        assert "nick=pocket2" in status()[1]

        console.serial.write(b"irc disconnect \t\t")
        completion = console.collect(1)
        assert name in completion, completion
        assert not re.search(r"\b(gateway|meshcore|link)\b", completion), completion
        # Backspace also works on the dumb USB shell; allow for a completed name.
        console.serial.write(b"\b" * 192)
        console.collect(0.6)
        console.command("")
        tasks = console.command("top")
        display(f"irc remove {name}")
        assert name not in console.command("irc status")
        print(json.dumps({"result": "pass", "instance": instance,
            "display_session": args.display_session, "tasks": tasks}, indent=2))
    finally:
        console.serial.close()


if __name__ == "__main__":
    main()
