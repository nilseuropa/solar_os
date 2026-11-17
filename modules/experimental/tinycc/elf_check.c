/* Run the production SolarOS ELF validator on generated modules. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "solar_os_native_elf.h"

int main(int argc, char **argv)
{
    int failed = 0;
    for (int i = 1; i < argc; i++) {
        FILE *file = fopen(argv[i], "rb");
        long length;
        if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) <= 0 ||
            fseek(file, 0, SEEK_SET)) return 2;
        unsigned char *data = malloc(length);
        if (!data || fread(data, 1, length, file) != (size_t)length) return 2;
        fclose(file);
        char detail[256];
        solar_os_native_elf_info_t info;
        int result = solar_os_native_elf_validate(data, length,
            SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA, &info, detail, sizeof(detail));
        printf("%s: %s\n", argv[i], result ? detail : "valid SolarOS ELF");
        failed |= result != 0;
        free(data);
    }
    return failed;
}
