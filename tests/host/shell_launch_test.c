#include <assert.h>
#include <stdio.h>

#include "solar_os_shell_launch.h"

static int path_arg(int argc, char *argv[])
{
    return solar_os_shell_launch_path_arg(argv[0], argc, argv);
}

int main(void)
{
    static const char *const first_path_apps[] = {
        "edit", "hexedit", "notes", "writer", "sheet", "gameboy",
        "python", "lua", "files",
    };
    char *first_path[] = {NULL, "relative/file"};
    for (size_t i = 0; i < sizeof(first_path_apps) / sizeof(first_path_apps[0]); i++) {
        first_path[0] = (char *)first_path_apps[i];
        assert(path_arg(2, first_path) == 1);
    }

    char *python[] = {"python", "tools/check.py", "input.txt"};
    assert(path_arg(3, python) == 1);

    char *player_file[] = {"player", "audio/song.mp3"};
    char *player_tui_file[] = {"player", "--tui", "audio/song.mp3"};
    char *player_file_tui[] = {"player", "audio/song.mp3", "--tui"};
    char *recorder_file[] = {"recorder", "audio/note.wav"};
    char *recorder_tui_file[] = {"recorder", "--tui", "audio/note.wav"};
    char *recorder_tui[] = {"recorder", "--tui"};
    assert(path_arg(2, player_file) == 1);
    assert(path_arg(3, player_tui_file) == 2);
    assert(path_arg(3, player_file_tui) == 1);
    assert(path_arg(2, recorder_file) == 1);
    assert(path_arg(3, recorder_tui_file) == 2);
    assert(path_arg(2, recorder_tui) == -1);

    char *less_file[] = {"less", "notes/today.md"};
    char *less_manual[] = {"less", "man:storage"};
    assert(path_arg(2, less_file) == 1);
    assert(path_arg(2, less_manual) == -1);

    char *reader_file[] = {"reader", "books/manual.md"};
    char *reader_manual[] = {"reader", "man:storage"};
    char *reader_pager_file[] = {"reader", "--pager", "books/manual.md"};
    char *reader_file_pager[] = {"reader", "books/manual.md", "--pager"};
    char *reader_pager_manual[] = {"reader", "--pager", "man:storage"};
    assert(path_arg(2, reader_file) == 1);
    assert(path_arg(2, reader_manual) == -1);
    assert(path_arg(3, reader_pager_file) == 2);
    assert(path_arg(3, reader_file_pager) == 1);
    assert(path_arg(3, reader_pager_manual) == -1);

    char *plot[] = {"plot", "--rate", "500", "--file", "logs/data.csv"};
    assert(path_arg(5, plot) == 4);

    char *curl[] = {"curl", "-L", "-o", "downloads/page.html", "https://example.test"};
    assert(path_arg(5, curl) == 3);

    char *aplay_before[] = {"aplay", "-v", "80", "audio/song.mp3"};
    char *aplay_after[] = {"aplay", "audio/song.mp3", "-v", "80"};
    char *arecord[] = {"arecord", "-d", "10", "audio/note.wav"};
    char *arecord_input[] = {
        "arecord", "-i", "adc0.capture", "audio/note.wav",
    };
    assert(path_arg(4, aplay_before) == 3);
    assert(path_arg(4, aplay_after) == 1);
    assert(path_arg(4, arecord) == 3);
    assert(path_arg(4, arecord_input) == 3);

    char *view[] = {"view", "--actual", "images/photo.png"};
    assert(path_arg(3, view) == 2);

    char *agent_file[] = {"agent", "script", "python", "agents/task.py", "argument"};
    char *agent_source[] = {"agent", "script", "python", "-c", "print('ok')"};
    assert(path_arg(5, agent_file) == 3);
    assert(path_arg(5, agent_source) == -1);

    char *files[] = {"files", "projects"};
    char *launcher[] = {"files", "--launcher"};
    char *launcher_path[] = {"files", "--launcher", "projects"};
    assert(path_arg(2, files) == 1);
    assert(path_arg(2, launcher) == -1);
    assert(path_arg(3, launcher_path) == 2);

    assert(solar_os_shell_path_is_script("./startup.sh"));
    assert(solar_os_shell_path_is_script("MENU.SH"));
    assert(!solar_os_shell_path_is_script("script.py"));
    assert(!solar_os_shell_path_is_script("trash"));

    puts("shell_launch_test: ok");
    return 0;
}
