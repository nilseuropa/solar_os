#include <stdio.h>
#include "answer.h"

int main(void)
{
    uint32_t answer = library_answer();
    printf("PROJECT_BUILD_PASS: library=%u\n", answer);
    return answer == 42 ? 0 : 1;
}
