from pathlib import Path
import re
import subprocess
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]


def production_functions(source, names):
    functions = []
    for name in names:
        match = re.search(r"^[A-Za-z_][^;{}\n]*\b" + name + r"\([^;]*?\)\s*\{", source, re.M)
        if not match:
            raise AssertionError(name)
        start = match.end() - 1
        depth = 1
        end = start + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        functions.append(source[match.start():end])
    return "\n".join(functions)


class ForegroundPresenterTest(unittest.TestCase):
    def test_ordinary_buttons_keep_mechanical_debounce(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "solar_os_board.h").write_text(
                '#pragma once\n#include "solar_os_keys.h"\n'
                '#define SOLAR_OS_BOARD_HAS_BUTTONS 1\n'
                '#define SOLAR_OS_BOARD_BUTTONS { '
                '{.pin=4, .key=SOLAR_OS_KEY_UP, .active_low=true}, '
                '{.pin=5, .key=SOLAR_OS_KEY_ENTER, .emit_on_release=true} }\n')
            binary = Path(tmp, "buttons")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I", tmp, "-I", str(ROOT / "tests/host/gpio_controller_stubs"),
                "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src"), "-I", str(ROOT / "src/services"),
                "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host/buttons_test.c"),
                str(ROOT / "src/services/solar_os_input.c"),
                str(ROOT / "src/services/solar_os_keys.c"),
                str(ROOT / "src/services/solar_os_input_composition.c"),
                str(ROOT / "src/services/solar_os_buttons.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_legacy_motion_buttons_and_session_shortcuts(self):
        source = (ROOT / "src/main.c").read_text()
        board = tomllib.loads((ROOT / "boards/manifests/t_deck_plus.toml").read_text())
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "foreground_focus_dispatch.inc").write_text(production_functions(
                source, ("dispatch_input_key",)))
            Path(tmp, "tdeck_matrix.inc").write_text(production_functions(
                (ROOT / "src/services/solar_os_tdeck_keyboard.c").read_text(),
                ("key_usage", "process_matrix")))
            defines = board["defines"]
            # The reusable board-button implementation still supports legacy
            # motion mappings; T-Deck now uses the separate pointer driver/job.
            defines["SOLAR_OS_BOARD_BUTTONS"] = "{{.pin=GPIO_NUM_3,.key=SOLAR_OS_KEY_UP,.track_motion=true,.pull=SOLAR_OS_BUTTON_PULL_UP}," \
                "{.pin=GPIO_NUM_15,.key=SOLAR_OS_KEY_DOWN,.track_motion=true,.pull=SOLAR_OS_BUTTON_PULL_UP}," \
                "{.pin=GPIO_NUM_1,.key=SOLAR_OS_KEY_LEFT,.track_motion=true,.pull=SOLAR_OS_BUTTON_PULL_UP}," \
                "{.pin=GPIO_NUM_2,.key=SOLAR_OS_KEY_RIGHT,.track_motion=true,.pull=SOLAR_OS_BUTTON_PULL_UP}}"
            Path(tmp, "solar_os_board.h").write_text(
                '#pragma once\n#include "solar_os_keys.h"\n'
                '#define SOLAR_OS_BOARD_HAS_BUTTONS 1\n' +
                ''.join(f'#define GPIO_NUM_{pin} {pin}\n' for pin in (1, 2, 3, 15)) +
                ''.join(f'#define {name} {defines[name]}\n' for name in (
                    "SOLAR_OS_BOARD_BUTTONS", "SOLAR_OS_BOARD_INPUT_CHORDS",
                    "SOLAR_OS_BOARD_TDECK_KEYMAP")))
            binary = Path(tmp, "trackball")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-DTDECK_TRACKBALL_TEST",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I", tmp, "-I", str(ROOT / "tests/host/gpio_controller_stubs"),
                "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src"), "-I", str(ROOT / "src/services"),
                "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host/foreground_focus_overlay_test.c"),
                str(ROOT / "src/services/solar_os_input.c"),
                str(ROOT / "src/services/solar_os_keys.c"),
                str(ROOT / "src/services/solar_os_input_composition.c"),
                str(ROOT / "src/services/solar_os_buttons.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_foreground_capture_closes_focus_overlay(self):
        source = (ROOT / "src/main.c").read_text()
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "foreground_focus_dispatch.inc").write_text(production_functions(
                source, ("dispatch_input_key",)))
            Path(tmp, "cl32_publish.inc").write_text(production_functions(
                (ROOT / "src/services/solar_os_cl32_core.c").read_text(),
                ("publish_wire_event",)))
            binary = Path(tmp, "focus")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-DCL32_KEYBOARD_TEST",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I", tmp, "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src"), "-I", str(ROOT / "src/services"),
                "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host/foreground_focus_overlay_test.c"),
                str(ROOT / "src/services/solar_os_input.c"),
                str(ROOT / "src/services/solar_os_keys.c"),
                str(ROOT / "src/services/solar_os_cl32_keyboard.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_presenter_suspension_reports_join_and_policy_failures(self):
        source = (ROOT / "src/services/solar_os_frame_presenter.c").read_text()
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "frame_lifecycle.inc").write_text(production_functions(
                source, ("solar_os_frame_presenter_suspend_checked", "solar_os_frame_presenter_redraw")))
            binary = Path(tmp, "presenter")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", tmp, "-I", str(ROOT / "tests/host"),
                str(ROOT / "tests/host/frame_presenter_lifecycle_test.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_literal_graphics_palette_and_monochrome_polarity(self):
        source = (ROOT / "src/services/solar_os_frame_presenter.c").read_text()
        functions = production_functions(source, (
            "frame_palette_index", "rgb565_luma", "frame_convert_native_mono_row", "frame_convert_indexed_to_mono", "frame_convert_rgb565_to_mono", "frame_convert_to_mono",
            "frame_prepare_mono_palette", "frame_present"))
        bayer = re.search(r"static const uint8_t bayer4\[4\]\[4\] = \{.*?\};",
                          source, re.S).group()
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "frame_palette.inc").write_text(bayer + "\n" + functions)
            binary = Path(tmp, "palette")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", tmp, "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src/services"),
                str(ROOT / "tests/host/frame_presenter_palette_test.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_presenter_fits_sources_larger_than_the_display(self):
        source = (ROOT / "src/services/solar_os_frame_presenter.c").read_text()
        state = re.search(r"struct solar_os_frame_presenter \{.*?\n\};",
                          source, re.S).group()
        bayer = re.search(r"static const uint8_t bayer4\[4\]\[4\] = \{.*?\};",
                          source, re.S).group()
        functions = production_functions(source, (
            "frame_minimum_stride", "frame_palette_valid", "frame_palette_index",
            "rgb565_luma", "frame_convert_native_mono_row", "frame_convert_indexed_to_mono", "frame_convert_rgb565_to_mono", "frame_convert_to_mono", "frame_prepare_mono_palette",
            "frame_present", "frame_choose_output", "frame_prepare_mono_output",
            "solar_os_frame_presenter_init", "solar_os_frame_presenter_resume",
            "solar_os_frame_presenter_deinit", "solar_os_frame_presenter_redraw",
            "solar_os_frame_presenter_submit", "solar_os_frame_presenter_acquire",
            "solar_os_frame_presenter_exchange"))
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "frame_fitting.inc").write_text(
                state + "\n" + bayer + "\n" + functions)
            binary = Path(tmp, "fitting")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", tmp, "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src"),
                "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "components/u8g2/src/clib"),
                str(ROOT / "tests/host/frame_presenter_fitting_test.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_foreground_abi_headers_compile_in_c_and_cpp(self):
        code = ('#include <stddef.h>\n#include "solar_os_native_app_abi.h"\n'
                '#define PREFIX_END (offsetof(solar_os_native_foreground_api_v1_t, set_exit_code) '
                '+ sizeof(((solar_os_native_foreground_api_v1_t *)0)->set_exit_code))\n'
                'typedef char append_only[offsetof(solar_os_native_foreground_api_v1_t, '
                'stop_requested) == PREFIX_END ? 1 : -1];\n'
                'typedef char suspend_append_only[offsetof(solar_os_native_foreground_api_v1_t, '
                'enable_suspend) == offsetof(solar_os_native_foreground_api_v1_t, resolve_path) + '
                'sizeof(((solar_os_native_foreground_api_v1_t *)0)->resolve_path) ? 1 : -1];\n')
        with tempfile.TemporaryDirectory() as tmp:
            for compiler, suffix, standard in (("cc", "c", "c11"), ("c++", "cpp", "c++17")):
                path = Path(tmp, "client." + suffix)
                path.write_text(code)
                subprocess.run([compiler, "-std=" + standard, "-Wall", "-Wextra", "-Werror",
                                "-pedantic", "-I", str(ROOT / "include"),
                                "-fsyntax-only", str(path)], check=True)


if __name__ == "__main__":
    unittest.main()
