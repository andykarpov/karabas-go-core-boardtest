// The tile screen (rtl/video/bt_tiles.v): 80 x 30 cells of a tile and two of 16 colours. The drawing
// helpers of the look approved in Ф0 (docs/img/f0_mock_*.png, the model tools/mockup.py).
#pragma once
#include <stdint.h>
#include "atlas_gen.h"

// the colours (palette entries); 0 is transparent (the gradient shows through)
enum {
  C_NONE = 0, C_TEXT, C_TEXT_A, C_FOOT, C_DIM, C_BUTTON, C_ACTIVE, C_PASS, C_FAIL, C_RUN, C_IDLE, C_LINE,
  C_LOGO, C_BLACK, C_PILL_T, C_COUNT
};

// the colours of the [ft812] section of karabas.ini (the theme of the boot menu)
typedef struct { uint32_t bg, gradient, button, active, text, text_active, footer; } look_t;
extern look_t look;

extern int scr_mute;                // no drawing (a group runs in the background: its page is not shown)
void scr_init(void);                 // the palette and the gradient from look, the screen cleared, shown
void scr_clear(void);
void scr_put(int c, int r, uint16_t tile, int fg, int bg);
int scr_text(int c, int r, const char *s, int fg, int bg);   // returns the column after it
void scr_text_n(int c, int r, const char *s, int n, int fg, int bg);  // exactly n cells (cut or padded)
void scr_fill(int c, int r, int w, int bg);
// a button of one row: rounded ends, the text centred (or left), a status dot cell (dot >= 0: its
// colour, -1 an empty dot cell, -2 no dot cell)
void scr_button(int c, int r, int w, const char *s, int color, int fg, int left, int dot);
void scr_pill(int c, int r, const char *s, int color);   // a button as wide as its text + 2
void scr_uline(int c, int r, int w, int fg);
void scr_meter(int c, int r, int w, int permille, int fg);
void scr_logo(int c, int r, int fg, int go);  // go: the logo of the Go (no "mini")
