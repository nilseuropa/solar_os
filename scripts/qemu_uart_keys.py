#!/usr/bin/env python3
"""qemu_uart_keys.py PORT: send this console's keys and mouse wheel to QEMU's UART1 on 127.0.0.1:PORT.

SolarOS's `uart-keyboard` driver on the qemu_esp32s3_rgb board reads them as a
VT100 terminal and hands them to the display shell. The mouse wheel over this
window sends Page Up (forward) and Page Down (back). Ctrl+C ends this script.
Windows only (console input API); see doc/manual/qemu.md for Linux and macOS.
Start QEMU with
    -serial tcp:127.0.0.1:PORT,server=on,wait=off
as its second -serial option.
"""
import ctypes
import socket
import sys
from ctypes import wintypes

KEY_EVENT = 0x0001
MOUSE_EVENT = 0x0002
MOUSE_WHEELED = 0x0004
ENABLE_MOUSE_INPUT = 0x0010
ENABLE_EXTENDED_FLAGS = 0x0080  # without ENABLE_QUICK_EDIT_MODE, so the mouse reaches us
STD_INPUT_HANDLE = -10

PAGE_UP = b"\x1b[5~"
PAGE_DOWN = b"\x1b[6~"
# Windows virtual-key codes of the keys that have no character.
SPECIAL_KEYS = {
    0x26: b"\x1b[A",   # Up
    0x28: b"\x1b[B",   # Down
    0x27: b"\x1b[C",   # Right
    0x25: b"\x1b[D",   # Left
    0x24: b"\x1b[H",   # Home
    0x23: b"\x1b[F",   # End
    0x2E: b"\x1b[3~",  # Del
    0x21: PAGE_UP,
    0x22: PAGE_DOWN,
}
CTRL_C = "\x03"


class KEY_EVENT_RECORD(ctypes.Structure):
    _fields_ = [
        ("bKeyDown", wintypes.BOOL),
        ("wRepeatCount", wintypes.WORD),
        ("wVirtualKeyCode", wintypes.WORD),
        ("wVirtualScanCode", wintypes.WORD),
        ("UnicodeChar", wintypes.WCHAR),
        ("dwControlKeyState", wintypes.DWORD),
    ]


class MOUSE_EVENT_RECORD(ctypes.Structure):
    _fields_ = [
        ("dwMousePosition", wintypes._COORD),
        ("dwButtonState", wintypes.DWORD),
        ("dwControlKeyState", wintypes.DWORD),
        ("dwEventFlags", wintypes.DWORD),
    ]


class _EVENT(ctypes.Union):
    _fields_ = [("KeyEvent", KEY_EVENT_RECORD), ("MouseEvent", MOUSE_EVENT_RECORD)]


class INPUT_RECORD(ctypes.Structure):
    _fields_ = [("EventType", wintypes.WORD), ("Event", _EVENT)]


def key_bytes(char: str, vk: int) -> bytes:
    """Bytes to send for one key press: VT100 for special keys, else the character as UTF-8."""
    if vk in SPECIAL_KEYS:
        return SPECIAL_KEYS[vk]
    if char == "\0":
        return b""
    # shortcut: a character outside the BMP arrives as two halves and is dropped; join them if SolarOS ever needs emoji.
    return char.encode("utf-8", errors="ignore")


def wheel_bytes(button_state: int) -> bytes:
    """Page Up for a forward wheel turn, Page Down for a backward one (high word, signed)."""
    delta = ctypes.c_short(button_state >> 16).value
    if delta > 0:
        return PAGE_UP
    if delta < 0:
        return PAGE_DOWN
    return b""


def record_bytes(record: INPUT_RECORD) -> bytes | None:
    """Bytes to send for one console input record; None means Ctrl+C (end)."""
    if record.EventType == KEY_EVENT:
        key = record.Event.KeyEvent
        if not key.bKeyDown:
            return b""
        if key.UnicodeChar == CTRL_C:
            return None
        return key_bytes(key.UnicodeChar, key.wVirtualKeyCode) * max(key.wRepeatCount, 1)
    if record.EventType == MOUSE_EVENT and record.Event.MouseEvent.dwEventFlags == MOUSE_WHEELED:
        return wheel_bytes(record.Event.MouseEvent.dwButtonState)
    return b""


def main() -> int:
    if len(sys.argv) != 2 or not sys.argv[1].isdigit():
        print("usage: qemu_uart_keys.py PORT")
        return 2
    port = int(sys.argv[1])
    try:
        conn = socket.create_connection(("127.0.0.1", port), timeout=5)
    except OSError as exc:
        print(f"cannot reach QEMU on 127.0.0.1:{port}: {exc}")
        return 1

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetStdHandle.restype = wintypes.HANDLE
    kernel32.GetConsoleMode.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel32.SetConsoleMode.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.ReadConsoleInputW.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(INPUT_RECORD), wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    handle = kernel32.GetStdHandle(STD_INPUT_HANDLE)
    old_mode = wintypes.DWORD()
    if not kernel32.GetConsoleMode(handle, ctypes.byref(old_mode)):
        print("qemu_uart_keys.py needs a console window for its input")
        return 2

    print(f"Keys and mouse wheel go to SolarOS keyboard0 (QEMU UART1, port {port}). Ctrl+C ends.")
    kernel32.SetConsoleMode(handle, ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT)
    record = INPUT_RECORD()
    count = wintypes.DWORD()
    try:
        with conn:
            while True:
                if not kernel32.ReadConsoleInputW(handle, ctypes.byref(record), 1, ctypes.byref(count)):
                    print(f"cannot read console input: error {ctypes.get_last_error()}")
                    return 1
                data = record_bytes(record)
                if data is None:
                    return 0
                if data:
                    try:
                        conn.sendall(data)
                    except OSError as exc:
                        print(f"QEMU closed the connection: {exc}")
                        return 1
    finally:
        kernel32.SetConsoleMode(handle, old_mode)


if __name__ == "__main__":
    sys.exit(main())
