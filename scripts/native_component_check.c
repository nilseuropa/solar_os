/* Validate packaged output with the same parser used before OS execution. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_elf.h"

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    FILE *file = fopen(argv[1], "rb");
    long length;
    if (!file) return 2;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET)) { fclose(file); return 2; }
    unsigned char *data = malloc((size_t)length);
    if (!data) { fclose(file); return 2; }
    bool read = fread(data, 1, (size_t)length, file) == (size_t)length;
    fclose(file);
    if (!read) { free(data); return 2; }
    char detail[256];
    solar_os_native_elf_info_t info;
    int result = solar_os_native_elf_validate(data, (size_t)length,
        SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA, &info, detail, sizeof(detail));
    free(data);
    if (result) { fprintf(stderr, "%s\n", detail); return 1; }
    if (info.foreground != (strcmp(argv[3], "2") == 0) ||
        (info.foreground && strcmp(info.app.id, argv[2]))) {
        fprintf(stderr, "ELF lifecycle or identity differs from module.toml\n");
        return 1;
    }
    return 0;
}
