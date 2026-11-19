#ifndef SOLAR_OS_C_STDIO_H
#define SOLAR_OS_C_STDIO_H
#include <stddef.h>
#include <stdarg.h>
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
typedef struct solar_os_c_file FILE;
extern FILE *stdout,*stderr;
int printf(const char *,...);
int vprintf(const char *,va_list);
int fprintf(FILE *,const char *,...);
int vfprintf(FILE *,const char *,va_list);
int snprintf(char *,size_t,const char *,...);
int vsnprintf(char *,size_t,const char *,va_list);
int puts(const char *);
int fputs(const char *,FILE *);
int putchar(int);
int fputc(int,FILE *);
int fgetc(FILE *);
FILE *fopen(const char *,const char *);
int fclose(FILE *);
int fflush(FILE *);
size_t fread(void *,size_t,size_t,FILE *);
size_t fwrite(const void *,size_t,size_t,FILE *);
int fseek(FILE *,long,int);
long ftell(FILE *);
int feof(FILE *);
int ferror(FILE *);
void clearerr(FILE *);
void rewind(FILE *);
#endif
