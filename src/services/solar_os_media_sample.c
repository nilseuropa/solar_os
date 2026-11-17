#include "solar_os_media_sample.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "solar_os_memory.h"

struct solar_os_media_sample {
    uint32_t references;
    bool is_audio;
    union {
        solar_os_media_sample_image_t image;
        solar_os_media_sample_audio_t audio;
    };
    char source[SOLAR_OS_MEDIA_SAMPLE_SOURCE_MAX];
    uint8_t bytes[];
};

static const char *image_type(const uint8_t *data, size_t length)
{
    if (length >= 3 && !memcmp(data, "\xff\xd8\xff", 3)) return "image/jpeg";
    if (length >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) return "image/png";
    if (length >= 6 && (!memcmp(data, "GIF87a", 6) || !memcmp(data, "GIF89a", 6))) return "image/gif";
    if (length >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4)) return "image/webp";
    if (length >= 2 && !memcmp(data, "BM", 2)) return "image/bmp";
    return NULL;
}

static esp_err_t allocate(size_t length, const solar_os_media_sample_metadata_t *metadata,
    solar_os_media_sample_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!length || length > SOLAR_OS_MEDIA_SAMPLE_MAX_BYTES) return ESP_ERR_INVALID_SIZE;
    if (metadata && metadata->source && strlen(metadata->source) >= SOLAR_OS_MEDIA_SAMPLE_SOURCE_MAX)
        return ESP_ERR_INVALID_SIZE;
    solar_os_media_sample_t *sample = solar_os_memory_calloc(1, sizeof(*sample) + length,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "media.sample");
    if (!sample) return ESP_ERR_NO_MEM;
    sample->references = 1;
    if (metadata) sample->image.metadata = *metadata;
    if (metadata && metadata->source) strcpy(sample->source, metadata->source);
    sample->image.metadata.source = sample->source;
    sample->image.data = sample->bytes;
    sample->image.length = length;
    *out = sample;
    return ESP_OK;
}

esp_err_t solar_os_media_sample_copy_image(const void *data, size_t length,
    const solar_os_media_sample_metadata_t *metadata, solar_os_media_sample_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!data) return ESP_ERR_INVALID_ARG;
    esp_err_t error = allocate(length, metadata, out);
    if (error) return error;
    memcpy((*out)->bytes, data, length);
    (*out)->image.mime_type = image_type((*out)->bytes, length);
    return ESP_OK;
}

static esp_err_t open_bounded(const char *path, size_t max_bytes,
    solar_os_media_sample_cancel_fn cancel, void *user, solar_os_media_sample_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!path || !*path) return ESP_ERR_INVALID_ARG;
    if (cancel && cancel(user)) return ESP_ERR_INVALID_STATE;
    struct stat status;
    if (stat(path, &status) || !S_ISREG(status.st_mode)) return ESP_ERR_NOT_FOUND;
    if (status.st_size <= 0 || (uint64_t)status.st_size > SOLAR_OS_MEDIA_SAMPLE_MAX_BYTES ||
        (uint64_t)status.st_size > max_bytes)
        return ESP_ERR_INVALID_SIZE;
    const solar_os_media_sample_metadata_t metadata = {.source = path};
    solar_os_media_sample_t *sample = NULL;
    esp_err_t error = allocate((size_t)status.st_size, &metadata, &sample);
    if (error) return error;
    FILE *file = fopen(path, "rb");
    if (!file) { solar_os_media_sample_release(sample); return ESP_FAIL; }
    size_t offset = 0;
    while (!error && offset < sample->image.length) {
        if (cancel && cancel(user)) { error = ESP_ERR_INVALID_STATE; break; }
        size_t chunk = sample->image.length - offset;
        if (chunk > 4096U) chunk = 4096U;
        if (fread(sample->bytes + offset, 1, chunk, file) != chunk) error = ESP_FAIL;
        offset += chunk;
    }
    if (!error && (fgetc(file) != EOF || ferror(file))) error = ESP_FAIL;
    if (fclose(file) && !error) error = ESP_FAIL;
    if (!error && cancel && cancel(user)) error = ESP_ERR_INVALID_STATE;
    if (error) { solar_os_media_sample_release(sample); return error; }
    sample->image.mime_type = image_type(sample->bytes, sample->image.length);
    *out = sample;
    return ESP_OK;
}

esp_err_t solar_os_media_sample_open_image(const char *path, solar_os_media_sample_t **out)
{ return solar_os_media_sample_open_image_bounded(path, SOLAR_OS_MEDIA_SAMPLE_MAX_BYTES, out); }
esp_err_t solar_os_media_sample_open_image_bounded(const char *path, size_t max_bytes,
    solar_os_media_sample_t **out)
{ return open_bounded(path, max_bytes, NULL, NULL, out); }

static const char *audio_format(const uint8_t *data, size_t length)
{
    if (length >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WAVE", 4)) return "wav";
    if (length >= 10 && !memcmp(data, "ID3", 3) && data[3] >= 2 && data[3] <= 4 &&
        !(data[6] & 0x80) && !(data[7] & 0x80) && !(data[8] & 0x80) && !(data[9] & 0x80)) return "mp3";
    if (length >= 4 && data[0] == 0xff && (data[1] & 0xe0) == 0xe0 &&
        (data[1] & 0x18) != 0x08 && (data[1] & 0x06) != 0 &&
        (data[2] & 0xf0) != 0xf0 && (data[2] & 0x0c) != 0x0c) return "mp3";
    return NULL;
}

static esp_err_t as_audio(solar_os_media_sample_t **sample)
{
    const char *format = audio_format((*sample)->bytes, (*sample)->image.length);
    if (!format) {
        solar_os_media_sample_release(*sample);
        *sample = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }
    solar_os_media_sample_audio_t audio = {
        .data = (*sample)->bytes, .length = (*sample)->image.length,
        .format = format, .metadata = (*sample)->image.metadata,
    };
    (*sample)->audio = audio;
    (*sample)->is_audio = true;
    return ESP_OK;
}

esp_err_t solar_os_media_sample_copy_audio(const void *data, size_t length,
    const solar_os_media_sample_metadata_t *metadata, solar_os_media_sample_t **out)
{
    esp_err_t error = solar_os_media_sample_copy_image(data, length, metadata, out);
    return error ? error : as_audio(out);
}

esp_err_t solar_os_media_sample_open_audio_bounded(const char *path, size_t max_bytes,
    solar_os_media_sample_cancel_fn cancel, void *user, solar_os_media_sample_t **out)
{
    esp_err_t error = open_bounded(path, max_bytes, cancel, user, out);
    return error ? error : as_audio(out);
}

const solar_os_media_sample_image_t *solar_os_media_sample_image(const solar_os_media_sample_t *sample)
{ return sample && !sample->is_audio ? &sample->image : NULL; }
const solar_os_media_sample_audio_t *solar_os_media_sample_audio(const solar_os_media_sample_t *sample)
{ return sample && sample->is_audio ? &sample->audio : NULL; }
void solar_os_media_sample_retain(solar_os_media_sample_t *sample)
{ if (sample) (void)__atomic_add_fetch(&sample->references, 1U, __ATOMIC_RELAXED); }
void solar_os_media_sample_release(solar_os_media_sample_t *sample)
{
    if (sample && !__atomic_sub_fetch(&sample->references, 1U, __ATOMIC_ACQ_REL))
        solar_os_memory_free(sample);
}
