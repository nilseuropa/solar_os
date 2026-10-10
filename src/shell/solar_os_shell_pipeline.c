#include "solar_os_shell_commands.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_pipeline.h"
#include "solar_os_memory.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static bool number(const char *text, uint32_t *out)
{
    if (!text || !*text || *text == '-') return false;
    char *end; errno = 0; unsigned long n = strtoul(text, &end, 10);
    if (errno || *end || n > 0x1fffffffUL) return false;
    *out = n; return true;
}
static void status(solar_os_shell_io_t *io, const solar_os_pipeline_status_t *s)
{
    solar_os_shell_io_printf(io, "%lu %s %s processor=%s model=%lu\n", (unsigned long)s->id,
        s->job, s->state, s->processor, (unsigned long)s->model);
    solar_os_shell_io_write(io, "source="); solar_os_shell_io_writeln(io, s->source);
    solar_os_shell_io_printf(io, "frames=%llu busy_frames=%llu sequence=%llu done=%s error=%s\n",
        (unsigned long long)s->frames, (unsigned long long)s->busy_frames,
        (unsigned long long)s->sequence, s->done ? "true" : "false", esp_err_to_name(s->last_error));
    solar_os_shell_io_printf(io, "decode_us=%llu process_us=%llu frame_us=%llu\n",
        (unsigned long long)s->decode_us, (unsigned long long)s->process_us, (unsigned long long)s->frame_us);
}
void solar_os_shell_cmd_pipeline(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    esp_err_t error = ESP_ERR_INVALID_ARG;
    uint32_t id = 0;
    if (argc == 2 && !strcmp(argv[1], "list")) {
        uint32_t ids[SOLAR_OS_PIPELINES_MAX]; size_t count = 0;
        error = solar_os_pipeline_list(ids, SOLAR_OS_PIPELINES_MAX, &count);
        for (size_t i = 0; !error && i < count; ++i) {
            solar_os_pipeline_status_t s; error = solar_os_pipeline_status(ids[i], &s);
            if (!error) status(io, &s);
        }
        if (!error && !count) solar_os_shell_io_writeln(io, "No pipelines");
    } else if (argc >= 4 && argc <= 7 && !strcmp(argv[1], "start")) {
        solar_os_pipeline_config_t c = {.processor = argv[2], .source = argv[3], .timeout_ms = 10000};
        int index = 4;
        if (!strcmp(c.processor, "model")) {
            if (argc <= index || !number(argv[index++], &c.model) || !c.model) goto done;
        }
        if (argc > index && !number(argv[index++], &c.limit)) goto done;
        if (argc > index && !number(argv[index++], &c.interval_ms)) goto done;
        if (argc != index) goto done;
        char path[256];
        if (!strncmp(c.source, "rtsp://", 7)) { /* Already an absolute URL. */ }
        else if (c.source[0] == '/' || c.source[0] == '.' || strchr(c.source, '/') ||
                 strstr(c.source, ".png") || strstr(c.source, ".jpg") || strstr(c.source, ".jpeg") ||
                 strstr(c.source, ".bmp") || strstr(c.source, ".gif") || strstr(c.source, ".webp")) {
            error = solar_os_shell_resolve_path(ctx, c.source, path, sizeof(path));
            if (error) goto done;
            c.source = path;
        }
        error = solar_os_pipeline_start(&c, &id);
        if (!error) solar_os_shell_io_printf(io, "Pipeline %lu (pipeline-%lu)\n", (unsigned long)id, (unsigned long)id);
    } else if (argc == 3 && number(argv[2], &id) && id) {
        if (!strcmp(argv[1], "stop")) error = solar_os_pipeline_stop(id);
        else if (!strcmp(argv[1], "destroy")) error = solar_os_pipeline_destroy(id);
        else if (!strcmp(argv[1], "status")) {
            solar_os_pipeline_status_t s; error = solar_os_pipeline_status(id, &s);
            if (!error) status(io, &s);
        } else if (!strcmp(argv[1], "result")) {
            char *json = NULL; error = solar_os_pipeline_result(id, 0, &json);
            if (!error) solar_os_shell_io_writeln(io, json ? json : "No result");
            solar_os_memory_free(json);
        }
    }
done:
    if (error) solar_os_shell_diag_esp(io, "pipeline", error, NULL, NULL);
}
