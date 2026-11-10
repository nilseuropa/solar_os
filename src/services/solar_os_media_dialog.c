#include "solar_os_media_widgets.h"

#include <ctype.h>
#include <string.h>
#include "solar_os_keys.h"

solar_os_media_input_action_t solar_os_media_dialog_input_key(
    char *text, size_t capacity, size_t *length, uint8_t key)
{
    if (!text || !length || !capacity || *length >= capacity) return SOLAR_OS_MEDIA_INPUT_NONE;
    if (key == SOLAR_OS_KEY_ESCAPE) return SOLAR_OS_MEDIA_INPUT_CANCEL;
    if (key == '\r' || key == '\n') return SOLAR_OS_MEDIA_INPUT_SUBMIT;
    if (key == '\b' || key == 0x7fU || key == SOLAR_OS_KEY_DELETE) {
        if (!*length) return SOLAR_OS_MEDIA_INPUT_NONE;
        size_t previous = *length - 1U;
        while (previous && ((unsigned char)text[previous] & 0xc0U) == 0x80U) previous--;
        text[previous] = 0;
        *length = previous;
        return SOLAR_OS_MEDIA_INPUT_CHANGED;
    }
    if ((isprint(key) || key >= 0xa0U) && *length + 1U < capacity) {
        text[(*length)++] = (char)key;
        text[*length] = 0;
        return SOLAR_OS_MEDIA_INPUT_CHANGED;
    }
    return SOLAR_OS_MEDIA_INPUT_NONE;
}
