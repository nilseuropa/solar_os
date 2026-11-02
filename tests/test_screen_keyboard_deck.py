from pathlib import Path
import re
import string
import subprocess
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DeckSymbolsTest(unittest.TestCase):
    def test_palette_is_exactly_the_punctuation_missing_from_tdeck(self):
        board = tomllib.loads((ROOT / "boards/manifests/t_deck_plus.toml").read_text())
        keymap = board["defines"]["SOLAR_OS_BOARD_TDECK_KEYMAP"]
        available = {chr(int(code, 16)) for code in re.findall(r"0x[0-9a-fA-F]+", keymap)}
        missing = set(string.punctuation) - available
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "deck.c").write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "solar_os_screen_keyboard_layout.h"
#include "solar_os_keys.h"
int main(void) {
    const solar_os_screen_keyboard_layout_t *layout=solar_os_screen_keyboard_layout_find("grid");
    void *state=calloc(1,layout->state_size);
    layout->init(state,SOLAR_OS_SCREEN_KEYBOARD_MODE_DECK);
    (void)layout->resize(state,320,240);
    for (unsigned i=0;i<14;i++) {
        solar_os_screen_keyboard_action_t action=layout->key(state,'\r');
        assert(action.type==SOLAR_OS_SCREEN_KEYBOARD_EMIT);
        putchar(action.key);(void)layout->key(state,SOLAR_OS_KEY_RIGHT);
    }
    assert(layout->key(state,'\r').key=='=');
    free(state);return 0;
}
''')
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "tests/host"), "-idirafter", str(ROOT / "include"),
                "-I", str(ROOT / "src/services"), str(path / "deck.c"),
                str(ROOT / "src/services/solar_os_screen_keyboard_layout.c"),
                "-o", str(path / "deck")], check=True)
            result = subprocess.check_output([str(path / "deck")], text=True)
        self.assertEqual(len(result), 14)
        self.assertEqual(len(set(result)), 14)
        self.assertEqual(set(result), missing)
