from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "modules/jet/main"
SOURCES = sorted(str(path) for path in (MODULE / "vendor").glob("*.cpp"))


class JetFramebufferTest(unittest.TestCase):
    def test_effects_preserve_wire_order_and_filter_contracts(self):
        with tempfile.TemporaryDirectory(prefix="solaros-jet-effects-") as temporary:
            images=[]
            for swapped in (0,1):
                binary=Path(temporary)/f"effects-{swapped}"
                output=binary.with_suffix('.rgb565')
                subprocess.run(['g++','-std=c++17','-O1','-g','-fsanitize=address,undefined',
                    f'-DJET_FRAMEBUFFER_SWAP_BYTES={swapped}',f'-I{MODULE}',f'-I{MODULE / "vendor"}',
                    str(ROOT/'tests/host/jet_postfx_test.cpp'),str(MODULE/'vendor/PostFX.cpp'),'-o',str(binary)],
                    check=True,capture_output=True)
                subprocess.run([str(binary),str(output)],check=True,capture_output=True)
                images.append(output.read_bytes())
            native,wire=images
            self.assertEqual(len(native),7*8*8*2)
            self.assertEqual(native[::2],wire[1::2]);self.assertEqual(native[1::2],wire[::2])

    def test_python_signed_options_and_full_width_handles(self):
        with tempfile.TemporaryDirectory(prefix="solaros-jet-python-") as temporary:
            tmp=Path(temporary)
            source=(ROOT / "src/apps/solar_os_python_jet_surface.inc").read_text()
            (tmp / "jet_python_options.inc").write_text(production_functions(source,("python_jet_options",)))
            binary=tmp / "options"
            subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror",
                f"-I{tmp}",f"-I{ROOT / 'include'}",str(ROOT / "tests/host/jet_python_options_test.c"),
                "-o",str(binary)],check=True,capture_output=True)
            subprocess.run([str(binary)],check=True)

    def test_display_claim_excludes_status_and_restores_terminal(self):
        with tempfile.TemporaryDirectory(prefix="solaros-jet-display-") as temporary:
            tmp = Path(temporary)
            source = (ROOT / "src/services/solar_os_display.c").read_text()
            (tmp / "display_terminal_policy.inc").write_text(production_functions(source, (
                "solar_os_display_set_terminal_blocked", "solar_os_display_terminal_draw_state",
                "solar_os_display_release")))
            source = (ROOT / "src/services/solar_os_terminal.c").read_text()
            (tmp / "terminal_draw_policy.inc").write_text(production_functions(source, (
                "solar_os_terminal_needs_draw", "solar_os_terminal_draw")))
            binary = tmp / "display"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", f"-I{tmp}", f"-I{ROOT / 'tests/host/jet_stubs'}",
                str(ROOT / "tests/host/display_terminal_claim_test.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_provider_abi_and_allocation_failures(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("g++ unavailable")
        with tempfile.TemporaryDirectory(prefix="solaros-jet-library-") as temporary:
            binary = Path(temporary) / "jet-library"
            command = [compiler, "-std=c++17", "-O1", "-g", "-fsanitize=address,undefined",
                       "-fno-omit-frame-pointer", f"-I{ROOT / 'include'}",
                       f"-I{MODULE}", f"-I{MODULE / 'vendor'}",
                       str(ROOT / "tests/host/jet_library_test.cpp"),
                       str(MODULE / "engine.cpp"), *SOURCES, "-o", str(binary)]
            subprocess.run(command, check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, capture_output=True, text=True,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})

    def test_native_and_wire_order_render_the_same_images(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("g++ unavailable")
        with tempfile.TemporaryDirectory(prefix="solaros-jet-") as temporary:
            images = []
            for swapped, optimization, instancing in ((0, "-O1", 0), (0, "-O1", 1), (1, "-O1", 1), (1, "-O3", 1)):
                binary = Path(temporary) / f"jet-{swapped}-{optimization}-{instancing}"
                output = Path(temporary) / f"jet-{swapped}-{optimization}-{instancing}.rgb565"
                command = [compiler, "-std=c++17", optimization, "-g", "-fsanitize=address",
                           "-fno-omit-frame-pointer", f"-DJET_FRAMEBUFFER_SWAP_BYTES={swapped}",
                           f"-DJET_CONSTANT_COLOR_LIGHTING={swapped}",
                           f"-DJET_UNIFORM_BYTE_CLEAR={swapped}",
                           f"-DJET_MESH_INSTANCING={instancing}",
                           f"-DJET_RUNTIME_POSTFX={instancing}",
                           f"-I{MODULE}", f"-I{MODULE / 'vendor'}",
                           str(ROOT / "tests/host/jet_framebuffer_test.cpp"), *SOURCES, "-o", str(binary)]
                subprocess.run(command, check=True, capture_output=True, text=True)
                subprocess.run([str(binary), str(output)], check=True, capture_output=True,
                               text=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})
                images.append(output.read_bytes())
            self.assertEqual(len(images[0]), (144 + 1) * 64 * 48 * 2)
            baseline, native, wire, optimized_wire = images
            self.assertEqual(baseline,native)
            self.assertEqual(wire, optimized_wire)
            self.assertEqual(native[0::2], wire[1::2])
            self.assertEqual(native[1::2], wire[0::2])
            red = ((144 * 64 * 48) + 10 * 64 + 10) * 2
            self.assertEqual(wire[red:red + 2], b"\xf8\x00")
            # A non-symmetric background catches accidental no-op byte order.
            self.assertEqual(wire[:2], b"\x12\x34")

    def test_frontend_leases_lua_bindings_and_example(self):
        with tempfile.TemporaryDirectory(prefix="solaros-jet-runtime-") as temporary:
            tmp = Path(temporary)
            include = [f"-I{ROOT / 'tests/host/jet_stubs'}", f"-I{ROOT / 'include'}",
                       f"-I{ROOT / 'src/services'}", f"-I{ROOT / 'src/apps'}", f"-I{tmp}"]
            flags = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            source = (ROOT / "src/apps/solar_os_lua.c").read_text()
            (tmp / "jet_visibility.inc").write_text(production_functions(source, ("solua_jet_visibility",)))
            objects = []
            for path in [MODULE / "engine.cpp", *map(Path, SOURCES)]:
                obj = tmp / (path.stem + ".o")
                subprocess.run(["g++", "-std=c++17", *flags, *include,
                    f"-I{MODULE}", f"-I{MODULE / 'vendor'}", "-c", str(path), "-o", str(obj)], check=True, capture_output=True)
                objects.append(str(obj))
            frontend = tmp / "frontend.o"
            (tmp / "frontend.c").write_text((ROOT / "src/services/solar_os_jet.c").read_text())
            subprocess.run(["cc", "-std=c11", *flags, *include, "-include", str(ROOT / "tests/host/compat.h"),
                "-Wall", "-Wextra", "-Werror", "-c", str(tmp / "frontend.c"),
                "-o", str(frontend)], check=True, capture_output=True)
            objects.append(str(frontend))
            for lua in (False, True):
                fixture = tmp / "fixture.o"
                subprocess.run(["cc", "-std=c11", *flags, *include,
                    *( ["-DJET_TEST_NO_MAIN=1"] if lua else []), "-c",
                    str(ROOT / "tests/host/jet_runtime_test.c"), "-o", str(fixture)], check=True, capture_output=True)
                extra = [str(fixture)]
                if lua:
                    lua_dir = ROOT / "components/lua/lua/src"
                    for path in [ROOT / "tests/host/jet_lua_test.c", *sorted(lua_dir.glob("*.c"))]:
                        if path.name in ("lua.c", "luac.c"): continue
                        obj = tmp / ("lua-" + path.stem + ".o")
                        subprocess.run(["cc", "-std=gnu11", *flags, *include, f"-I{lua_dir}",
                            "-Wall", "-Wextra", "-Werror", "-c", str(path), "-o", str(obj)], check=True, capture_output=True)
                        extra.append(str(obj))
                binary = tmp / ("lua" if lua else "runtime")
                subprocess.run(["g++", *flags, *objects, *extra, "-lm", "-o", str(binary)], check=True, capture_output=True)
                subprocess.run([str(binary), *([str(ROOT / name) for name in
                    ("examples/lua/jet.lua", "examples/lua/jet_materials.lua", "examples/lua/jet_scene.lua",
                     "examples/lua/jet_picking.lua", "examples/lua/jet_assets.lua",
                     "examples/lua/jet_instances.lua", "examples/lua/jet_particles.lua",
                     "examples/lua/jet_effects.lua", "examples/lua/jet_tiles.lua",
                     "examples/lua/jet_shaders.lua", "examples/lua/jet_controls.lua")] if lua else [])],
                    check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})


if __name__ == "__main__":
    unittest.main()
