from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions


ROOT = Path(__file__).resolve().parents[1]
SERVICES = ROOT / "src/services"
LUA = ROOT / "components/lua/lua/src"

BOARD_HEADER = r'''
#define SOLAR_OS_BOARD_ID "test_board"
#define SOLAR_OS_BOARD_NAME "Test Board"
#ifdef TEST_PSRAM
#define SOLAR_OS_BOARD_MODULE_NAME "Test Module"
#define SOLAR_OS_BOARD_HAS_PSRAM 1
#define SOLAR_OS_BOARD_PSRAM_BYTES 8388608U
#define SOLAR_OS_BOARD_HAS_WIFI 1
#define SOLAR_OS_BOARD_HAS_CAMERA 1
#endif
'''

PYTHON_OBJECTS = r'''
typedef struct object *mp_obj_t;
typedef uint32_t mp_uint_t;
struct object {
    const char *text;
    uint32_t number;
    size_t count;
    const char *keys[64];
    mp_obj_t values[64];
};
#define mp_const_none NULL
static mp_obj_t object(void) {
    mp_obj_t value = calloc(1, sizeof(*value));
    assert(value != NULL);
    return value;
}
static mp_obj_t mp_obj_new_dict(size_t capacity) { (void)capacity; return object(); }
static mp_obj_t mp_obj_new_list(size_t count, const mp_obj_t *items) {
    assert(count == 0 && items == NULL); return object();
}
static mp_obj_t mp_obj_new_str_from_cstr(const char *text) {
    mp_obj_t value = object(); value->text = text; return value;
}
static mp_obj_t mp_obj_new_int_from_uint(mp_uint_t number) {
    mp_obj_t value = object(); value->number = number; return value;
}
static const char *python_key(const char *key) { return key; }
static void mp_obj_dict_store(mp_obj_t dict, const char *key, mp_obj_t value) {
    assert(dict->count < 64);
    dict->keys[dict->count] = key; dict->values[dict->count++] = value;
}
static void mp_obj_list_append(mp_obj_t list, mp_obj_t value) {
    assert(list->count < 64); list->values[list->count++] = value;
}
static mp_obj_t field(mp_obj_t dict, const char *key) {
    for (size_t i = 0; i < dict->count; i++)
        if (strcmp(dict->keys[i], key) == 0) return dict->values[i];
    assert(false); return NULL;
}
'''

PYTHON_CHECK = r'''
int main(void) {
    mp_obj_t info = solaros_board_info();
    assert(info->count == 5);
    assert(strcmp(field(info, "id")->text, "test_board") == 0);
    assert(strcmp(field(info, "name")->text, "Test Board") == 0);
    mp_obj_t caps = field(info, "capabilities");
#ifdef TEST_PSRAM
    assert(strcmp(field(info, "module")->text, "Test Module") == 0);
    assert(field(info, "psram_bytes")->number == 8388608);
    assert(caps->count == 3);
    const char *expected[] = {"wifi", "psram", "camera"};
    for (size_t i = 0; i < 3; i++) {
        bool found = false;
        for (size_t j = 0; j < caps->count; j++)
            found |= strcmp(caps->values[j]->text, expected[i]) == 0;
        assert(found);
    }
#else
    assert(strcmp(field(info, "module")->text, "") == 0);
    assert(field(info, "psram_bytes")->number == 0);
    assert(caps->count == 0);
#endif
    size_t count = caps->count;
    caps->count = 42;
    assert(field(solaros_board_info(), "capabilities")->count == count);
    return 0;
}
'''

LUA_CHECK = r'''
int main(void) {
    lua_State *L = luaL_newstate();
    assert(L != NULL);
    luaL_openlibs(L);
    lua_pushcfunction(L, solua_board_info);
    lua_setglobal(L, "board_info");
    const char *check =
        "local info = board_info(); "
        "assert(info.id == 'test_board' and info.name == 'Test Board'); "
        "local n = 0; for _ in pairs(info) do n = n + 1 end; assert(n == 5); "
#ifdef TEST_PSRAM
        "assert(info.module == 'Test Module' and info.psram_bytes == 8388608); "
        "assert(#info.capabilities == 3); "
        "local caps = {}; for _, v in ipairs(info.capabilities) do caps[v] = true end; "
        "assert(caps.wifi and caps.psram and caps.camera); "
#else
        "assert(info.module == '' and info.psram_bytes == 0); "
        "assert(#info.capabilities == 0); "
#endif
        "info.capabilities[1] = 'changed'; assert(board_info().capabilities[1] ~= 'changed')";
    int result = luaL_dostring(L, check);
    if (result != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(L, -1));
    assert(result == LUA_OK);
    lua_close(L);
    return 0;
}
'''


class ScriptBoardBindingsTest(unittest.TestCase):
    def run_binding(self, runtime, psram):
        source = (ROOT / f"src/apps/solar_os_{runtime}.c").read_text()
        harness = '#include "solar_os_board_caps.h"\n'
        harness += '#include <assert.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n'
        if runtime == "python":
            harness += PYTHON_OBJECTS
            harness += production_functions(source, (
                "python_dict_store_cstr", "python_dict_store_uint", "solaros_board_info"))
            harness += PYTHON_CHECK
        else:
            harness += '#include "lua.h"\n#include "lauxlib.h"\n#include "lualib.h"\n'
            harness += production_functions(source, (
                "solua_set_str", "solua_set_int", "solua_board_info"))
            harness += LUA_CHECK

        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / "solar_os_board.h").write_text(BOARD_HEADER)
            (folder / "test.c").write_text(harness)
            command = ["cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                       "-I", directory, "-I", str(SERVICES), "-I", str(LUA)]
            if psram:
                command.append("-DTEST_PSRAM")
            command += [str(folder / "test.c"), str(SERVICES / "solar_os_board_caps.c")]
            if runtime == "lua":
                command += [str(path) for path in LUA.glob("*.c")
                            if path.name not in {"lua.c", "luac.c"}]
            result = subprocess.run(command + ["-lm", "-ldl", "-o", str(folder / "test")],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(folder / "test")], check=True)

    def test_python_board_information(self):
        for psram in (False, True):
            with self.subTest(psram=psram):
                self.run_binding("python", psram)

    def test_lua_board_information_with_real_interpreter(self):
        for psram in (False, True):
            with self.subTest(psram=psram):
                self.run_binding("lua", psram)


if __name__ == "__main__":
    unittest.main()
