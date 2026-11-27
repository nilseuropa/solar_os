"""Run production game geometry and media drawing on small display targets."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class SmallDisplayAppsTest(unittest.TestCase):
    def test_synth_full_play_controls_stay_above_piano(self):
        source = (ROOT / "src/apps/solar_os_synth_app.c").read_text()
        declarations = source[source.index("typedef enum {"):source.index("typedef struct {\n  bool active;")]
        functions = production_functions(source, (
            "synth_add_touch_target", "synth_touch_target_at",
            "synth_envelope_value_index", "synth_filter_cutoff_value_index",
            "synth_draw_knob", "synth_draw_envelope", "synth_draw_volume_button",
            "synth_draw_piano", "synth_bottom_height", "synth_editor_bottom",
            "synth_play_graph_height", "synth_tab_button_rect", "synth_tab_at",
            "synth_tab_name", "synth_piano_note_at", "synth_draw_bottom", "synth_draw_header",
            "synth_use_compact_layout", "synth_use_micro_layout", "synth_draw_play",
            "synth_draw_filter_response", "synth_format_frequency", "synth_draw_filter_editor",
            "synth_draw_oscillator_panel", "synth_draw_oscillator2_editor",
            "synth_draw_wave_editor", "synth_draw_preset_editor", "synth_draw_mode_editor",
            "synth_draw_scope_trace", "synth_draw_scope"))
        functions = functions.replace("static void synth_draw_bottom(",
                                      "static void synth_draw_bottom_pixels(", 1)
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp / "synth_types.inc").write_text(declarations)
            (tmp / "synth_play_view.inc").write_text(functions)
            binary = tmp / "synth-view"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I" + str(tmp), "-I" + str(ROOT / "tests/host"),
                "-I" + str(ROOT / "src/services"), "-I" + str(ROOT / "include"),
                str(ROOT / "tests/host/synth_play_view_test.c"), "-lm", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_media_app_views(self):
        widgets = (ROOT / "src/services/solar_os_media_widgets.c").read_text()
        helpers = widgets[widgets.index("static void media_transport_triangle"):
                          widgets.index("void solar_os_cassette_widget_reset")]
        helpers += production_functions(widgets, ("solar_os_media_playlist_draw",))
        browser = (ROOT / "src/services/solar_os_storage_browser.c").read_text()
        helpers += production_functions(browser, ("solar_os_storage_browser_media_entry",))
        names = {
            "player": ("player_draw_header", "player_draw_visualizer", "player_render_play", "player_playlist_entry", "player_render_list"),
            "recorder": ("recorder_draw_header", "recorder_draw_visualizer", "recorder_progress_text",
                "recorder_draw_progress", "recorder_render_record_graphics", "recorder_render_setup_graphics",
                "recorder_render_browser_graphics", "recorder_draw_filename_dialog"),
            "webradio": ("webradio_draw_graphics_header", "webradio_draw_visualizer", "webradio_render_player",
                "webradio_dialog_label", "webradio_draw_dialog", "webradio_render_channels"),
            "vplay": ("viewport",),
        }
        for app, functions in names.items():
            helpers += production_functions((ROOT / f"src/apps/solar_os_{app}.c").read_text(), functions)
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp / "media_app_views.inc").write_text(helpers)
            binary = tmp / "media-views"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I" + str(tmp), "-I" + str(ROOT / "tests/host"), "-I" + str(ROOT / "src/services"), "-I" + str(ROOT / "include"),
                str(ROOT / "tests/host/media_app_views_test.c"), str(ROOT / "src/services/solar_os_gfx_widgets.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_invaders_gameplay_and_rendering(self):
        source = (ROOT / "src/apps/solar_os_invaders.c").read_text()
        declarations = source[source.index("#define INVADERS_ROWS"):
                              source.index("#if SOLAR_OS_PACKAGE_SERVICE_AUDIO\ntypedef struct")]
        functions = source[source.index("static int invaders_screen_w"):
                           source.index("static void invaders_render")]
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp / "invaders_geometry.inc").write_text(declarations + "\nstatic invaders_state_t invaders;\n"
                "static void invaders_sound_queue(invaders_sound_t sound) { (void)sound; }\n" + functions)
            binary = tmp / "invaders-test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I" + str(tmp), "-I" + str(ROOT / "tests/host"), "-I" + str(ROOT / "src/services"), "-I" + str(ROOT / "include"),
                str(ROOT / "tests/host/invaders_geometry_test.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
