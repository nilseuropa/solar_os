"""Key and wheel mapping of scripts/qemu_uart_keys.py."""
import importlib.util
from pathlib import Path
import sys
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("qemu_uart_keys", REPOSITORY / "scripts" / "qemu_uart_keys.py")
assert SPEC is not None and SPEC.loader is not None
qemu_uart_keys = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = qemu_uart_keys
SPEC.loader.exec_module(qemu_uart_keys)
KEY_EVENT = qemu_uart_keys.KEY_EVENT
MOUSE_EVENT = qemu_uart_keys.MOUSE_EVENT
MOUSE_WHEELED = qemu_uart_keys.MOUSE_WHEELED
INPUT_RECORD = qemu_uart_keys.INPUT_RECORD
key_bytes = qemu_uart_keys.key_bytes
record_bytes = qemu_uart_keys.record_bytes

MOUSE_MOVED = 0x0001


def key_record(char: str, vk: int, down: bool = True, repeat: int = 1) -> INPUT_RECORD:
    record = INPUT_RECORD()
    record.EventType = KEY_EVENT
    record.Event.KeyEvent.bKeyDown = down
    record.Event.KeyEvent.wRepeatCount = repeat
    record.Event.KeyEvent.wVirtualKeyCode = vk
    record.Event.KeyEvent.UnicodeChar = char
    return record


def mouse_record(flags: int, button_state: int) -> INPUT_RECORD:
    record = INPUT_RECORD()
    record.EventType = MOUSE_EVENT
    record.Event.MouseEvent.dwEventFlags = flags
    record.Event.MouseEvent.dwButtonState = button_state
    return record


class KeyBytesTest(unittest.TestCase):
    def test_arrow_becomes_vt100(self):
        self.assertEqual(key_bytes("\0", 0x26), b"\x1b[A")
        self.assertEqual(key_bytes("\0", 0x25), b"\x1b[D")

    def test_enter_backspace_and_app_exit_pass_through(self):
        self.assertEqual(key_bytes("\r", 0x0D), b"\r")
        self.assertEqual(key_bytes("\x08", 0x08), b"\x08")
        self.assertEqual(key_bytes("\x1d", 0xDD), b"\x1d")

    def test_key_without_character_sends_nothing(self):
        self.assertEqual(key_bytes("\0", 0x10), b"")  # Shift alone

    def test_text_is_utf8(self):
        self.assertEqual(key_bytes("ä", 0xDE), "ä".encode("utf-8"))


class RecordBytesTest(unittest.TestCase):
    def test_wheel_forward_is_page_up_back_is_page_down(self):
        self.assertEqual(record_bytes(mouse_record(MOUSE_WHEELED, 120 << 16)), b"\x1b[5~")
        self.assertEqual(record_bytes(mouse_record(MOUSE_WHEELED, (-120 & 0xFFFF) << 16)), b"\x1b[6~")

    def test_mouse_move_sends_nothing(self):
        self.assertEqual(record_bytes(mouse_record(MOUSE_MOVED, 0)), b"")

    def test_key_up_sends_nothing_and_repeat_repeats(self):
        self.assertEqual(record_bytes(key_record("a", 0x41, down=False)), b"")
        self.assertEqual(record_bytes(key_record("a", 0x41, repeat=3)), b"aaa")

    def test_ctrl_c_ends(self):
        self.assertIsNone(record_bytes(key_record("\x03", 0x43)))


if __name__ == "__main__":
    unittest.main()
