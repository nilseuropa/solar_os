#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void)
{
    if(fopen("missing.txt","rb") || fopen("data.bin","a")) return 1;
    unsigned char *data=malloc(70001),*copy=malloc(70001);
    if(!data || !copy) return 2;
    for(unsigned i=0;i<70001;i++) data[i]=(unsigned char)(i*17U);
    FILE *file=fopen("data.bin","wb");if(!file) return 3;
    if(fwrite(data,1,70001,file)!=70001 || ftell(file)!=70001 || fflush(NULL)) return 4;
    if(fseek(file,10,SEEK_SET) || fputc(42,file)!=42 || fclose(file)) return 5;
    data[10]=42;file=fopen("data.bin","rb");if(!file) return 6;
    if(fread(copy,1,70001,file)!=70001 || memcmp(data,copy,70001) || feof(file)) return 7;
    if(fgetc(file)!=EOF || !feof(file) || ferror(file)) return 8;
    rewind(file);if(feof(file) || ftell(file) || fgetc(file)!=data[0]) return 9;
    if(fseek(file,-1,SEEK_END) || fgetc(file)!=data[70000] || fclose(file)) return 10;
    file=fopen("limits.txt","wb");if(!file) return 11;
    if(fwrite("x",(size_t)-1,2,file) || !ferror(file)) return 12;
    clearerr(file);if(ferror(file) || fwrite("OK",1,2,file)!=2 || fclose(file)) return 13;
    FILE *slots[16];for(unsigned i=0;i<16;i++) {slots[i]=fopen("slots.txt","wb");if(!slots[i]) return 14;}
    if(fopen("slots.txt","wb")) return 15;
    for(unsigned i=0;i<16;i++) if(fclose(slots[i])) return 16;
    free(data);free(copy);
    file=fopen("auto.txt","wb");if(!file || fputs("automatic flush\n",file)==EOF) return 17;
    puts("SDK_IO_PASS");return 0; /* The native C entry closes file on return. */
}
