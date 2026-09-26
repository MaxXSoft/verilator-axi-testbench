#ifndef FUXI_BENCH_STDIO_H
#define FUXI_BENCH_STDIO_H
#include <stddef.h>
int printf(const char *format, ...);
int puts(const char *text);
char *strcpy(char *destination, const char *source);
int strcmp(const char *left, const char *right);
void *memcpy(void *destination, const void *source, size_t size);
void *memset(void *destination, int value, size_t size);
#endif
