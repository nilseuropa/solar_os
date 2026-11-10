#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_media_widgets.h"
#include "solar_os_keys.h"

int main(void)
{
    char text[6] = "";
    size_t len = 0;
    const char *word = "camera";
    for (const char *p = word; *p; p++) solar_os_media_dialog_input_key(text, sizeof(text), &len, *p);
    assert(!strcmp(text, "camer") && len == 5);
    assert(solar_os_media_dialog_input_key(text, sizeof(text), &len, '\n') == SOLAR_OS_MEDIA_INPUT_SUBMIT);
    assert(solar_os_media_dialog_input_key(text, sizeof(text), &len, SOLAR_OS_KEY_ESCAPE) == SOLAR_OS_MEDIA_INPUT_CANCEL);
    strcpy(text, "T\xc3\xb6ne"); len = strlen(text);
    solar_os_media_dialog_input_key(text, sizeof(text), &len, SOLAR_OS_KEY_DELETE);
    assert(!strcmp(text, "T\xc3\xb6n") && len == 4);
    solar_os_media_dialog_input_key(text, sizeof(text), &len, '\b');
    solar_os_media_dialog_input_key(text, sizeof(text), &len, '\b');
    assert(!strcmp(text, "T") && len == 1);
    assert(solar_os_media_dialog_input_key(text, sizeof(text), &len, SOLAR_OS_KEY_UP) == SOLAR_OS_MEDIA_INPUT_NONE);
    assert(solar_os_media_dialog_input_key(text, sizeof(text), &len, 'q') == SOLAR_OS_MEDIA_INPUT_CHANGED);
    assert(!strcmp(text, "Tq"));
    puts("Shared media dialog bounds, cancel, submit and UTF-8 deletion: passed");
}
