#pragma once
#include "solar_os_agent.h"

esp_err_t solar_os_agent_images_validate(solar_os_media_sample_t *const *images, size_t count);
esp_err_t solar_os_agent_audio_validate(const solar_os_media_sample_t *audio);
esp_err_t solar_os_agent_input_content(const char *prompt,
    solar_os_media_sample_t *const *images, size_t count,
    const solar_os_media_sample_t *audio, bool responses, char **content);
/* Produces a JSON content value, quoted text for text-only requests and typed
 * content parts with base64 data URLs for images. Caller frees the result. */
esp_err_t solar_os_agent_image_content(const char *prompt,
    solar_os_media_sample_t *const *images, size_t count, bool responses, char **content);
/* Extract leading --image SOURCE options (or /image in chat), preserving the
 * remaining prompt verbatim. Sources support shell-style quoting/escaping. */
esp_err_t solar_os_agent_image_options(char *prompt,
    char sources[SOLAR_OS_AGENT_IMAGES_MAX][SOLAR_OS_MEDIA_SAMPLE_SOURCE_MAX], size_t *count);
