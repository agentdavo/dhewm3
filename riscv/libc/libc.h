/*
 * Minimal freestanding C library for the dhewm3 RISC-V bare-metal target.
 *
 * This toolchain (riscv64-unknown-elf-gcc) ships without newlib, and a bare
 * metal target wants a self-contained, auditable libc anyway. Only the
 * freestanding headers (stdint/stddef/stdarg) are used; everything hosted is
 * provided here and grows as the engine needs it.
 */
#ifndef DHEWM3_RV_LIBC_H
#define DHEWM3_RV_LIBC_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* mem / string */
void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);

/* allocator (see malloc.c) */
void  *malloc(size_t n);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *ptr, size_t n);
void   free(void *ptr);

/* formatted output (routed to the SBI console) */
int    putchar(int c);
int    puts(const char *s);
int    printf(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
int    vprintf(const char *fmt, va_list ap);
int    snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int    vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

#endif /* DHEWM3_RV_LIBC_H */
