#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sha1.h"

int main(int argc, char **argv)
{
    assert(argc == 2);
    static const unsigned char expected[20] = {
        0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
        0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d
    };
    SHA1_CTX context;
    unsigned char digest[20];
    SHA1DCInit(&context);
    SHA1DCUpdate(&context, "abc", 3);
    assert(SHA1DCFinal(digest, &context) == 0);
    assert(memcmp(digest, expected, sizeof(digest)) == 0);

    FILE *file = fopen(argv[1], "rb");
    assert(file);
    SHA1DCInit(&context);
    char buffer[4096];
    size_t size;
    while ((size = fread(buffer, 1, sizeof(buffer), file)) != 0)
        SHA1DCUpdate(&context, buffer, size);
    assert(!ferror(file));
    assert(fclose(file) == 0);
    assert(SHA1DCFinal(digest, &context) != 0);
    return 0;
}
