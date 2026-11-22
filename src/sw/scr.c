// The tile screen: see scr.h.
#include "scr.h"
#include "mmio.h"
#include "util.h"

look_t look = { 0x010101, 0x0000c8, 0x202026, 0x0000c8, 0xffffff, 0xffffff, 0x787878 };  // Classic

#define CELL(t, fg, bg) ((uint32_t) (((bg) & 15) << 14 | ((fg) & 15) << 10 | ((t) & 0x3ff)))
#define OPAQUE(rgb) (0xF0000000u | ((rgb) & 0xFFFFFF))

void scr_clear(void) {
  for (int i = 0; i < COLS * ROWS; i++) R_CELL(i) = CELL(T_EMPTY, 0, 0);
}

void scr_init(void) {
  R_PAL(C_NONE) = 0;
  R_PAL(C_TEXT) = OPAQUE(look.text);
  R_PAL(C_TEXT_A) = OPAQUE(look.text_active);
  R_PAL(C_FOOT) = OPAQUE(look.footer);
  R_PAL(C_DIM) = OPAQUE(((look.text & 0xfefefe) >> 1) + ((look.footer & 0xfefefe) >> 1));   // between text and footer
  R_PAL(C_BUTTON) = OPAQUE(look.button);
  R_PAL(C_ACTIVE) = OPAQUE(look.active);
  R_PAL(C_PASS) = OPAQUE(0x2fbf5a);
  R_PAL(C_FAIL) = OPAQUE(0xe0303c);
  R_PAL(C_RUN) = OPAQUE(0xe0b000);
  R_PAL(C_IDLE) = OPAQUE(0x3a5058);
  R_PAL(C_LINE) = OPAQUE(0x6a8a92);
  R_PAL(C_LOGO) = OPAQUE(0xffffff);
  R_PAL(C_BLACK) = OPAQUE(0x000000);
  R_GRAD_TOP = look.gradient;
  R_GRAD_BOT = look.bg;
  scr_clear();
  R_SHOW = 1;
}

void scr_put(int c, int r, uint16_t tile, int fg, int bg) {
  if (c < 0 || c >= COLS || r < 0 || r >= ROWS) return;
  R_CELL(r * COLS + c) = CELL(tile, fg, bg);
}

static uint16_t font_tile(char ch) {
  return (ch >= 32 && ch < 127) ? t_font[ch - 32] : t_font['?' - 32];
}

int scr_text(int c, int r, const char *s, int fg, int bg) {
  for (; *s; s++, c++) scr_put(c, r, font_tile(*s), fg, bg);
  return c;
}

void scr_text_n(int c, int r, const char *s, int n, int fg, int bg) {
  for (int i = 0; i < n; i++) scr_put(c + i, r, *s ? font_tile(*s++) : T_EMPTY, fg, bg);
}

void scr_fill(int c, int r, int w, int bg) {
  for (int i = 0; i < w; i++) scr_put(c + i, r, T_EMPTY, C_TEXT, bg);
}

void scr_big(int c, int r, const char *s, int fg) {
  static const char chars[] = BIG_CHARS;
  for (; *s; s++, c += 2) {
    int k = 0;
    while (chars[k] && chars[k] != *s) k++;
    if (!chars[k]) k = sizeof(chars) - 2;   // the space
    scr_put(c, r, t_big[k][0], fg, C_NONE);
    scr_put(c + 1, r, t_big[k][1], fg, C_NONE);
    scr_put(c, r + 1, t_big[k][2], fg, C_NONE);
    scr_put(c + 1, r + 1, t_big[k][3], fg, C_NONE);
  }
}

void scr_button(int c, int r, int w, const char *s, int color, int fg, int left, int dot) {
  scr_put(c, r, T_END_L, color, C_NONE);
  scr_put(c + w - 1, r, T_END_R, color, C_NONE);
  scr_fill(c + 1, r, w - 2, color);
  int c0 = dot != -2 ? c + 2 : c + 1, n = dot != -2 ? w - 3 : w - 2;
  int len = str_len(s);
  if (len > n - (left ? 1 : 0)) len = n - (left ? 1 : 0);
  int x = c0 + (left ? 1 : (n - len) / 2);
  scr_text_n(x, r, s, len, fg, color);
  if (dot >= 0) scr_put(c + 1, r, T_SDOT, dot, color);
}

void scr_pill(int c, int r, const char *s, int color) {
  scr_button(c, r, str_len(s) + 2, s, color, C_TEXT_A, 0, -2);
}

void scr_uline(int c, int r, int w, int fg) {
  for (int i = 0; i < w; i++) scr_put(c + i, r, T_ULINE, fg, C_NONE);
}

void scr_meter(int c, int r, int w, int permille, int fg) {
  int n = permille * w * CELL_W / 1000;
  for (int i = 0; i < w; i++) {
    int k = n - i * CELL_W;
    k = k < 0 ? 0 : k > CELL_W ? CELL_W : k;
    scr_put(c + i, r, t_bar[k], fg, C_NONE);
  }
}

void scr_logo(int c, int r, int fg) {
  for (int y = 0; y < LOGO_ROWS; y++)
    for (int x = 0; x < LOGO_COLS; x++) scr_put(c + x, r + y, t_logo[y * LOGO_COLS + x], fg, C_NONE);
}
