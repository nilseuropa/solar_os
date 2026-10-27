#!/usr/bin/env python3
"""Load a Pico image after a 1200-baud SDK BOOTSEL reset, with identity checks."""
import argparse
import errno
from pathlib import Path
import re
import subprocess
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--port", type=Path, required=True)
    parser.add_argument("--flash-id", required=True)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    assert args.image.is_file()
    if args.port.exists():
        try:
            with serial.Serial(str(args.port), 1200, timeout=.1) as port:
                port.dtr = False
        except OSError as error:
            # The USB reset can disconnect while the control ioctl is pending.
            if error.errno not in (errno.EPROTO, errno.ENODEV, errno.EIO):
                raise
    deadline = time.monotonic() + 12
    while True:
        info = subprocess.run([str(args.tool), "info", "-a"], capture_output=True, text=True)
        if info.returncode == 0:
            ids = re.findall(r"flash id:\s*0x([0-9A-Fa-f]+)", info.stdout)
            assert ids == [args.flash_id.upper()] or [s.upper() for s in ids] == [args.flash_id.upper()], info.stdout
            print(info.stdout, flush=True)
            break
        if time.monotonic() >= deadline:
            raise RuntimeError(info.stdout + info.stderr)
        time.sleep(.2)
    subprocess.run([str(args.tool), "load", "-v", "-x", str(args.image)], check=True)
    deadline = time.monotonic() + 12
    while not args.port.exists():
        if time.monotonic() >= deadline:
            raise TimeoutError("Pico USB CDC did not reappear")
        time.sleep(.2)


if __name__ == "__main__":
    main()
