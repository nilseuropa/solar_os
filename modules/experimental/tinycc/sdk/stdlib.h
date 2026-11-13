#ifndef SOLAR_OS_C_STDLIB_H
#define SOLAR_OS_C_STDLIB_H
#include <stddef.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
void *malloc(size_t);
void *calloc(size_t,size_t);
void *realloc(void *,size_t);
void free(void *);
int abs(int);
long labs(long);
int atoi(const char *);
double atof(const char *);
#endif
