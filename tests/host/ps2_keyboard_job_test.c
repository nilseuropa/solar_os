#define main input_service_tests
#include "input_test.c"
#undef main

#include "solar_os_expansion.h"
#include "solar_os_ps2_keyboard_job.h"

static solar_os_input_source_t job_source;
static char attachment_name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];

esp_err_t solar_os_expansion_attach(
    const char *driver,
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    assert(strcmp(driver, "ps2-keyboard") == 0);
    assert(binding_count == 1);
    assert(bindings[0].kind == SOLAR_OS_EXPANSION_BINDING_PS2_BUS);
    assert(strcmp(bindings[0].target, "ps2kbd") == 0);

    /* The real keyboard attachment registers its device name as a source. */
    const esp_err_t err = solar_os_input_keyboard_source_open(name, true, &job_source);
    if (err == ESP_OK) {
        strlcpy(attachment_name, name, sizeof(attachment_name));
    }
    return err;
}

esp_err_t solar_os_expansion_detach(const char *name)
{
    assert(attachment_name[0] != '\0');
    assert(strcmp(name, attachment_name) == 0);
    solar_os_input_source_close(job_source);
    attachment_name[0] = '\0';
    return ESP_OK;
}

int main(void)
{
    assert(solar_os_input_init() == ESP_OK);
    char *args[] = {"ps2-keyboard", "ps2kbd"};

    for (unsigned i = 0; i < 2; i++) {
        assert(solar_os_ps2_keyboard_job.start(NULL, 2, args) == ESP_OK);
        assert(solar_os_input_keyboard_count() == 1);
        solar_os_input_source_info_t info;
        assert(solar_os_input_source_get(0, &info));
        assert(strcmp(info.name, attachment_name) == 0);
        assert(info.source_class == SOLAR_OS_INPUT_SOURCE_KEYBOARD);
        assert(info.ready);

        solar_os_ps2_keyboard_job.stop(NULL);
        assert(solar_os_input_keyboard_count() == 0);
        assert(attachment_name[0] == '\0');
    }

    puts("ps2 keyboard job registration and restart tests: ok");
    return 0;
}
