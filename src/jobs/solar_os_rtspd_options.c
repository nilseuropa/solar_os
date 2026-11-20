#include "solar_os_rtspd_options.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool number(const char *text, unsigned maximum, unsigned *value)
{
    if (!text || !*text) return false;
    for (const char *p = text; *p; p++) {
        if (!isdigit((unsigned char)*p)) return false;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (*end || parsed > maximum) return false;
    *value = (unsigned)parsed;
    return true;
}

bool solar_os_rtspd_parse_options(int argc, char **argv,
                                   solar_os_rtspd_options_t *options)
{
    if (!options || argc < 0 || (argc && !argv)) return false;
    *options = (solar_os_rtspd_options_t) {
        .video = true,
        .video_source = "camera0",
        /* Zero dimensions select the source's advertised default. */
        .camera = {0},
        .fps = 5U,
        .port = 554U,
    };
    unsigned seen = 0U;
    const int first = argc && argv[0] && !strcmp(argv[0], "rtspd") ? 1 : 0;
    for (int i = first; i < argc; i++) {
        const char *arg = argv[i];
        unsigned bit = 0U, value = 0U;
        if (!arg) return false;
        if (!strncmp(arg, "video=", 6)) {
            bit = 1U;
            const char *id = arg + 6;
            if (!*id || strlen(id) >= sizeof(options->video_source)) return false;
            for (const char *p = id; *p; p++) {
                if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' &&
                    *p != '.') return false;
            }
            options->video = strcmp(id, "none") != 0;
            strcpy(options->video_source, !strcmp(id, "camera") ? "camera0" : id);
        } else if (!strncmp(arg, "audio=", 6)) {
            bit = 2U;
            const char *id = arg + 6;
            if (!*id || strlen(id) >= sizeof(options->audio)) return false;
            for (const char *p = id; *p; p++) {
                if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' &&
                    *p != '.') return false;
            }
            if (strcmp(id, "none")) strcpy(options->audio, id);
        } else if (!strncmp(arg, "size=", 5)) {
            bit = 4U;
            if (!solar_os_camera_parse_resolution(arg + 5, &options->camera.width,
                                                   &options->camera.height)) return false;
        } else if (!strncmp(arg, "fps=", 4)) {
            bit = 8U;
            if (!number(arg + 4, 30U, &value)) return false;
            options->fps = (uint8_t)value;
        } else if (!strncmp(arg, "port=", 5)) {
            bit = 16U;
            if (!number(arg + 5, UINT16_MAX, &value) || !value) return false;
            options->port = (uint16_t)value;
        } else return false;
        if (seen & bit) return false;
        seen |= bit;
    }
    return (options->video || options->audio[0]) &&
        (options->video || !(seen & (4U | 8U)));
}

bool solar_os_rtspd_video_due(uint64_t captured_us, uint64_t origin_us,
                               uint64_t last_sent_us, uint8_t fps)
{
    if (captured_us < origin_us || (last_sent_us && captured_us <= last_sent_us))
        return false;
    return !fps || !last_sent_us || captured_us - last_sent_us >= 1000000U / fps;
}
