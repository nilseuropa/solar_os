/* SPDX-License-Identifier: LGPL-2.0-or-later */
#include <stdio.h>
#include "libtcc.h"

int main(int argc, char **argv)
{
    TCCState *state;
    int i, result;
    if (argc < 4) {
        fprintf(stderr, "usage: solar-tcc INPUT.c OUTPUT.o INCLUDE_DIR...\n");
        return 2;
    }
    state = tcc_new();
    if (!state) return 1;
    tcc_set_options(state, "-nostdinc -nostdlib -fno-asynchronous-unwind-tables -fno-common");
    for (i = 3; i < argc; i++) tcc_add_include_path(state, argv[i]);
    result = tcc_set_output_type(state, TCC_OUTPUT_OBJ);
    if (!result) result = tcc_add_file(state, argv[1]);
    if (!result) result = tcc_output_file(state, argv[2]);
    tcc_delete(state);
    return result != 0;
}
