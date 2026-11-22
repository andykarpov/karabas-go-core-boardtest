// Small string helpers: see util.h.
#include "util.h"

int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

char *str_cpy(char *d, const char *s) { while ((*d = *s++)) d++; return d; }

char *fmt_u(char *d, uint32_t v) {
  char t[10];
  int n = 0;
  do { t[n++] = (char) ('0' + v % 10); v /= 10; } while (v);
  while (n) *d++ = t[--n];
  *d = 0;
  return d;
}

char *fmt_u2(char *d, uint32_t v) {
  *d++ = (char) ('0' + v / 10 % 10);
  *d++ = (char) ('0' + v % 10);
  *d = 0;
  return d;
}

char *fmt_hex(char *d, uint32_t v, int digits) {
  for (int i = digits - 1; i >= 0; i--) *d++ = "0123456789ABCDEF"[(v >> (4 * i)) & 15];
  *d = 0;
  return d;
}

int parse_hex(const char *s, uint32_t *v) {
  if (*s == '#') s++;
  else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  uint32_t x = 0;
  int n = 0;
  for (; *s; s++, n++) {
    char c = *s;
    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0 || n >= 8) return 0;
    x = x << 4 | (uint32_t) d;
  }
  if (!n) return 0;
  *v = x;
  return 1;
}

void *memset(void *d, int c, size_t n) {
  uint8_t *p = (uint8_t *) d;
  while (n--) *p++ = (uint8_t) c;
  return d;
}

void *memcpy(void *d, const void *s, size_t n) {
  uint8_t *p = (uint8_t *) d;
  const uint8_t *q = (const uint8_t *) s;
  while (n--) *p++ = *q++;
  return d;
}
