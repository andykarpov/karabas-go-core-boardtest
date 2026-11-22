// Small string helpers (no libc in the program: it has to fit the block RAM).
#pragma once
#include <stdint.h>
#include <stddef.h>

int str_len(const char *s);
char *str_cpy(char *d, const char *s);          // returns the end (the NUL)
char *fmt_u(char *d, uint32_t v);               // decimal
char *fmt_u2(char *d, uint32_t v);              // two digits
char *fmt_hex(char *d, uint32_t v, int digits);
int parse_hex(const char *s, uint32_t *v);      // "RRGGBB" or "#RRGGBB" or "0xRRGGBB"
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
