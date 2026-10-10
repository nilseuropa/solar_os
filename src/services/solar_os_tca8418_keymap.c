#include "solar_os_tca8418.h"
#include "solar_os_input_keymap.h"
esp_err_t solar_os_tca8418_load_keymap(const char *name, const char *path)
{
    return solar_os_input_keymap_load(name, path);
}
