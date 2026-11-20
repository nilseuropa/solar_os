#include <stdio.h>
#include <math.h>
#include <string.h>
#include "project.h"
int main(void)
{
    uint32_t answer=library_add(19,23);
    if(answer!=42) return 1;
    FILE *file=fopen("result.txt","wb");if(!file) return 2;
    if(fprintf(file,"library=%u sqrt=%.2f\n",answer,(double)sqrtf(81.0f))<0 || fclose(file)) return 3;
    file=fopen("result.txt","rb");if(!file) return 4;
    char text[64]={0};size_t bytes=fread(text,1,sizeof(text)-1,file);
    if(ferror(file) || fclose(file) || bytes!=21 || strcmp(text,"library=42 sqrt=9.00\n")) return 5;
    printf("SDK_PROJECT_PASS %s",text);
    return 0;
}
