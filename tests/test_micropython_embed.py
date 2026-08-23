import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
GENERATOR_PATH = REPOSITORY / "scripts" / "generate_micropython_embed.py"
SPEC = importlib.util.spec_from_file_location("generate_micropython_embed", GENERATOR_PATH)
assert SPEC is not None and SPEC.loader is not None
generate_micropython_embed = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = generate_micropython_embed
SPEC.loader.exec_module(generate_micropython_embed)


class MicroPythonEmbedTest(unittest.TestCase):
    def test_selected_standard_modules_are_registered(self):
        moduledefs = (
            generate_micropython_embed.PACKAGE / "genhdr" / "moduledefs.h"
        ).read_text(encoding="utf-8")
        for module in (
            "ARRAY",
            "BINASCII",
            "CMATH",
            "COLLECTIONS",
            "ERRNO",
            "GC",
            "HASHLIB",
            "IO",
            "JSON",
            "MATH",
            "MICROPYTHON",
            "RANDOM",
            "STRUCT",
            "SYS",
        ):
            self.assertIn(f"MODULE_DEF_{module}", moduledefs)

    def test_selected_extmod_sources_are_vendored(self):
        for relative in (
            generate_micropython_embed.SELECTED_EXTMOD_SOURCES
            + generate_micropython_embed.SELECTED_EXTMOD_SUPPORT
        ):
            self.assertTrue((generate_micropython_embed.PACKAGE / relative).is_file())

    def test_public_io_module_and_file_types_are_generated(self):
        source = (
            generate_micropython_embed.PACKAGE / "py" / "objstringio.c"
        ).read_text(encoding="utf-8")
        self.assertIn("#if MICROPY_PY_IO", source)
        moduledefs = (
            generate_micropython_embed.PACKAGE / "genhdr" / "moduledefs.h"
        ).read_text(encoding="utf-8")
        self.assertIn("MODULE_DEF_IO", moduledefs)
        qstrs = (
            generate_micropython_embed.PACKAGE / "genhdr" / "qstrdefs.generated.h"
        ).read_text(encoding="utf-8")
        self.assertIn("MP_QSTR_FileIO", qstrs)
        self.assertIn("MP_QSTR_TextIOWrapper", qstrs)

    def test_selected_standard_modules_are_enabled(self):
        config = (
            generate_micropython_embed.COMPONENT / "mpconfigport.h"
        ).read_text(encoding="utf-8")
        self.assertIn("MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES", config)
        for feature in (
            "MICROPY_PY_JSON",
            "MICROPY_PY_BINASCII",
            "MICROPY_PY_HASHLIB",
            "MICROPY_PY_HASHLIB_SHA256",
            "MICROPY_PY_RANDOM",
            "MICROPY_PY_MATH",
            "MICROPY_PY_STRUCT",
            "MICROPY_PY_COLLECTIONS",
            "MICROPY_ENABLE_EXTERNAL_IMPORT",
            "MICROPY_PY_IO",
            "MICROPY_READER_POSIX",
        ):
            self.assertRegex(config, rf"#define {feature}\s+\(1\)")

    def test_extra_profile_keeps_unintegrated_port_features_disabled(self):
        config = (
            generate_micropython_embed.COMPONENT / "mpconfigport.h"
        ).read_text(encoding="utf-8")
        for feature in (
            "MICROPY_PY_BUILTINS_EXECFILE",
            "MICROPY_PY_BUILTINS_INPUT",
            "MICROPY_PY_SYS_STDFILES",
            "MICROPY_PY_ASYNCIO",
            "MICROPY_PY_OS",
            "MICROPY_PY_HASHLIB_MD5",
            "MICROPY_PY_HASHLIB_SHA1",
            "MICROPY_PY_LWIP",
            "MICROPY_PY_SSL",
            "MICROPY_PY_WEBSOCKET",
        ):
            self.assertRegex(config, rf"#define {feature}\s+\(0\)")

    def test_extra_profile_initializes_c_stack_limit(self):
        config = (
            generate_micropython_embed.COMPONENT / "mpconfigport.h"
        ).read_text(encoding="utf-8")
        self.assertRegex(
            config,
            r"#define MICROPY_STACK_CHECK_MARGIN\s+\(1024U\)",
        )
        source = (
            generate_micropython_embed.PORT_OVERRIDES / "embed_util.c"
        ).read_text(encoding="utf-8")
        self.assertIn(
            "mp_cstack_init_with_top(stack_top, uxTaskGetStackHighWaterMark(NULL));",
            source,
        )

    def test_compiler_roots_active_parse_tree(self):
        source = (
            generate_micropython_embed.PACKAGE / "py" / "compile.c"
        ).read_text(encoding="utf-8")
        roots = (
            generate_micropython_embed.PACKAGE / "genhdr" / "root_pointers.h"
        ).read_text(encoding="utf-8")
        override = (
            generate_micropython_embed.PORT_OVERRIDES / "embed_util.c"
        ).read_text(encoding="utf-8")
        self.assertIn(
            "MP_REGISTER_ROOT_POINTER(void *solar_os_active_parse_tree_chunk);",
            source,
        )
        self.assertIn(
            "MP_STATE_VM(solar_os_active_parse_tree_chunk) = parse_tree->chunk;",
            source,
        )
        self.assertIn(
            "MP_STATE_VM(solar_os_active_parse_tree_chunk) = NULL;",
            source,
        )
        self.assertIn("void *solar_os_active_parse_tree_chunk;", roots)
        self.assertIn(
            "MP_STATE_VM(solar_os_active_parse_tree_chunk) = NULL;",
            override,
        )

    def test_native_i64_values_use_small_int_aware_conversion(self):
        source_paths = [REPOSITORY / "src/apps/solar_os_python.c"]
        source_paths.extend(
            sorted((REPOSITORY / "src/apps").glob("solar_os_python_*.inc"))
        )
        source = "\n".join(
            path.read_text(encoding="utf-8") for path in source_paths
        )
        self.assertIn(
            "return python_u64_to_obj(solar_os_time_uptime_ms());",
            source,
        )
        self.assertIn(
            "return python_i64_to_obj(value);",
            source,
        )
        self.assertEqual(source.count("mp_obj_new_int_from_ll("), 1)
        self.assertEqual(source.count("mp_obj_new_int_from_ull("), 1)

    def test_import_reader_uses_solaros_path_resolution(self):
        source = (
            generate_micropython_embed.PACKAGE / "py" / "reader.c"
        ).read_text(encoding="utf-8")
        self.assertIn("solar_os_micropython_resolve_path", source)
        self.assertIn("int fd = open(resolved, O_RDONLY, 0644);", source)

    def test_port_provides_solaros_open_and_import_stat(self):
        source = (
            generate_micropython_embed.PORT_OVERRIDES / "solaros_file.c"
        ).read_text(encoding="utf-8")
        self.assertIn("mp_obj_t mp_builtin_open", source)
        self.assertIn("mp_import_stat_t mp_import_stat", source)
        self.assertIn("O_EXCL", source)
        self.assertIn("solar_os_micropython_stop_requested", source)

    def test_solaros_port_overrides_match_vendored_output(self):
        for override in generate_micropython_embed.PORT_OVERRIDES.iterdir():
            if override.is_file():
                self.assertEqual(
                    override.read_bytes(),
                    (
                        generate_micropython_embed.PACKAGE
                        / "port"
                        / override.name
                    ).read_bytes(),
                )

    def test_tree_difference_reports_missing_unexpected_and_changed_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            expected = root / "expected"
            actual = root / "actual"
            expected.mkdir()
            actual.mkdir()
            (expected / "missing").write_text("expected", encoding="utf-8")
            (expected / "changed").write_text("expected", encoding="utf-8")
            (actual / "changed").write_text("actual", encoding="utf-8")
            (actual / "unexpected").write_text("actual", encoding="utf-8")
            self.assertEqual(
                generate_micropython_embed.differences(expected, actual),
                ["missing missing", "unexpected unexpected", "changed changed"],
            )


if __name__ == "__main__":
    unittest.main()
