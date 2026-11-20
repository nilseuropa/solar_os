#include "solar_os_agent_images.h"
#include "solar_os_memory.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

esp_err_t solar_os_agent_images_validate(solar_os_media_sample_t *const *images, size_t count)
{
    if (count > SOLAR_OS_AGENT_IMAGES_MAX || (count && !images)) return ESP_ERR_INVALID_ARG;
    size_t bytes = 0;
    for (size_t i = 0; i < count; i++) {
        const solar_os_media_sample_image_t *image = solar_os_media_sample_image(images[i]);
        if (!image || !image->mime_type) return ESP_ERR_NOT_SUPPORTED;
        if (strcmp(image->mime_type,"image/jpeg") && strcmp(image->mime_type,"image/png") &&
            strcmp(image->mime_type,"image/gif") && strcmp(image->mime_type,"image/webp")) return ESP_ERR_NOT_SUPPORTED;
        if (image->length > SOLAR_OS_AGENT_IMAGE_BYTES_MAX - bytes) return ESP_ERR_INVALID_SIZE;
        bytes += image->length;
    }
    return ESP_OK;
}

esp_err_t solar_os_agent_audio_validate(const solar_os_media_sample_t *sample)
{
    if (!sample) return ESP_OK;
    const solar_os_media_sample_audio_t *audio = solar_os_media_sample_audio(sample);
    if (!audio || !audio->format ||
        (strcmp(audio->format, "wav") && strcmp(audio->format, "mp3"))) return ESP_ERR_NOT_SUPPORTED;
    return audio->length > SOLAR_OS_AGENT_AUDIO_BYTES_MAX ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static char *escaped_text(char *out, const char *text)
{
    static const char hex[] = "0123456789abcdef";
    *out++ = '"';
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == '"' || *p == '\\') { *out++ = '\\'; *out++ = *p; }
        else if (*p < 32) {
            memcpy(out,"\\u00",4); out += 4;
            *out++ = hex[*p >> 4]; *out++ = hex[*p & 15];
        } else *out++ = *p;
    }
    *out++ = '"'; *out = 0; return out;
}
static char *base64(char *out, const uint8_t *data, size_t length)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < length; i += 3) {
        uint32_t word = (uint32_t)data[i] << 16;
        if (i + 1 < length) word |= (uint32_t)data[i+1] << 8;
        if (i + 2 < length) word |= data[i+2];
        *out++ = alphabet[word >> 18]; *out++ = alphabet[(word >> 12) & 63];
        *out++ = i+1 < length ? alphabet[(word >> 6) & 63] : '=';
        *out++ = i+2 < length ? alphabet[word & 63] : '=';
    }
    *out = 0; return out;
}
esp_err_t solar_os_agent_image_content(const char *prompt,
    solar_os_media_sample_t *const *images, size_t count, bool responses, char **content)
{
    return solar_os_agent_input_content(prompt, images, count, NULL, responses, content);
}

esp_err_t solar_os_agent_input_content(const char *prompt,
    solar_os_media_sample_t *const *images, size_t count,
    const solar_os_media_sample_t *sample, bool responses, char **content)
{
    if (!content) return ESP_ERR_INVALID_ARG;
    *content = NULL;
    if (!prompt || strlen(prompt) >= SOLAR_OS_AGENT_PROMPT_MAX) return ESP_ERR_INVALID_SIZE;
    esp_err_t error = solar_os_agent_images_validate(images,count);
    if (error) return error;
    error = solar_os_agent_audio_validate(sample);
    if (error) return error;
    if (sample && responses) return ESP_ERR_NOT_SUPPORTED;
    const solar_os_media_sample_audio_t *audio = solar_os_media_sample_audio(sample);
    size_t capacity = strlen(prompt)*6 + 256;
    for (size_t i = 0; i < count; i++) {
        const solar_os_media_sample_image_t *image = solar_os_media_sample_image(images[i]);
        capacity += 4*((image->length+2)/3) + 128;
    }
    if (audio) capacity += 4*((audio->length+2)/3) + 128;
    char *buffer = solar_os_memory_alloc(capacity,SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,"agent.image-content");
    if (!buffer) return ESP_ERR_NO_MEM;
    char *p = buffer;
    if (count || audio) p += sprintf(p,"[{\"type\":\"%s\",\"text\":",responses ? "input_text" : "text");
    p = escaped_text(p,prompt);
    if (count || audio) *p++ = '}';
    for (size_t i = 0; i < count; i++) {
        const solar_os_media_sample_image_t *image = solar_os_media_sample_image(images[i]);
        p += sprintf(p,responses ? ",{\"type\":\"input_image\",\"image_url\":\"data:%s;base64,"
            : ",{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:%s;base64,",image->mime_type);
        p = base64(p,image->data,image->length);
        memcpy(p,responses ? "\"}" : "\"}}",responses ? 2 : 3); p += responses ? 2 : 3;
    }
    if (audio) {
        p += sprintf(p, ",{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"");
        p = base64(p, audio->data, audio->length);
        p += sprintf(p, "\",\"format\":\"%s\"}}", audio->format);
    }
    if (count || audio) *p++ = ']';
    *p = 0; *content = buffer;
    return ESP_OK;
}

static esp_err_t source_word(const char **input, char *out, size_t capacity)
{
    const char *p = *input; size_t length = 0; char quote = 0;
    while (isspace((unsigned char)*p)) p++;
    while (*p && (quote || !isspace((unsigned char)*p))) {
        char c = *p++;
        if (c == '\\' && quote != '\'') {
            if (!*p) return ESP_ERR_INVALID_ARG;
            c = *p++;
        } else if (c == '\'' || c == '"') {
            if (!quote) { quote = c; continue; }
            if (quote == c) { quote = 0; continue; }
        }
        if (length + 1 >= capacity) return ESP_ERR_INVALID_SIZE;
        out[length++] = c;
    }
    if (quote || !length) return ESP_ERR_INVALID_ARG;
    out[length] = 0; *input = p; return ESP_OK;
}
esp_err_t solar_os_agent_image_options(char *prompt,
    char sources[SOLAR_OS_AGENT_IMAGES_MAX][SOLAR_OS_MEDIA_SAMPLE_SOURCE_MAX], size_t *count)
{
    if (!prompt || !sources || !count) return ESP_ERR_INVALID_ARG;
    *count = 0; const char *p = prompt;
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        size_t flag = !strncmp(p,"--image",7) ? 7 : !strncmp(p,"/image",6) ? 6 : 0;
        if (!flag || (p[flag] && !isspace((unsigned char)p[flag]))) break;
        if (*count >= SOLAR_OS_AGENT_IMAGES_MAX) return ESP_ERR_INVALID_SIZE;
        p += flag;
        esp_err_t error = source_word(&p,sources[*count],SOLAR_OS_MEDIA_SAMPLE_SOURCE_MAX);
        if (error) return error;
        ++*count;
    }
    if (*count) {
        if (!strncmp(p,"--",2) && isspace((unsigned char)p[2])) {
            p += 2; while (isspace((unsigned char)*p)) p++;
        }
        if (!*p) return ESP_ERR_INVALID_ARG;
        memmove(prompt,p,strlen(p)+1);
    }
    return ESP_OK;
}
