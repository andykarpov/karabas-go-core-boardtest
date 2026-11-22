// The program of the board test CPU (rtl/soc/bt_soc.v), Ф0: the look of the main screen (docs/img/f0_mock_main.png)
// with live data: the colours of karabas.ini, the board and the firmware build (RPC SYS INFO), the clock (RPC SYS
// TIME_GET), the keys, the mouse (the cursor) and the frame counter. The groups have no tests yet (Ф1-Ф4).
#include "mmio.h"
#include "rpc.h"
#include "scr.h"
#include "util.h"

static uint8_t buf[RPC_MAX];

static void dbg(const char *s) { while (*s) R_DBG = (uint8_t) *s++; }

// the [ft812] colours of karabas.ini (the theme of the boot menu); the Classic ones if a value is missing
static void load_look(void) {
  static const char *keys[7] = { "color_bg", "color_gradient", "color_button", "color_active", "color_text",
                                 "color_text_active", "color_copyright" };
  uint32_t *dst = &look.bg;
  char req[200], *p = req;
  p = str_cpy(p, "0") + 1;
  for (int i = 0; i < 7; i++) { p = str_cpy(p, "ft812") + 1; p = str_cpy(p, keys[i]) + 1; }
  uint16_t n;
  if (rpc_call(RPC_CFG, 0, req, (uint16_t) (p - req), buf, sizeof(buf) - 1, &n, 2000) != RPC_OK) return;
  buf[n] = 0;
  const char *q = (const char *) buf, *end = (const char *) buf + n;
  for (int i = 0; i < 7 && q < end; i++) {
    int found = *q++;
    uint32_t v;
    if (found && parse_hex(q, &v)) dst[i] = v & 0xFFFFFF;
    while (q < end && *q) q++;
    q++;
  }
}

static const struct { const char *name; int na; } groups[] = {
  { "FPGA / MCU", 0 }, { "SRAM", 0 }, { "SDRAM", 0 }, { "SPI flash", 1 }, { "SD cards", 0 }, { "CF / IDE", 0 },
  { "FT812", 0 }, { "Wi-Fi", 0 }, { "Audio", 0 }, { "Tape", 0 }, { "MIDI", 0 }, { "Video", 0 }, { "Input", 0 },
  { "Floppy", 0 },
};
#define NGROUPS ((int) (sizeof(groups) / sizeof(groups[0])))
#define HALF 38

static int sel;

static void draw_group(int i) {
  int col = i < 7 ? 1 : 2 + HALF, row = 8 + 2 * (i % 7);
  scr_button(col, row, 14, groups[i].name, i == sel ? C_ACTIVE : C_BUTTON, C_TEXT_A, 1, -2);
  scr_pill(col + 15, row, groups[i].na ? "N/A " : " -- ", C_IDLE);
  scr_text_n(col + 22, row, groups[i].na ? "not fitted" : "not run", HALF - 23, C_DIM, C_NONE);
}

static void draw_main(const char *board, const char *fw) {
  scr_clear();
  scr_logo(0, 0, C_LOGO);
  scr_text(1, 5, "Karabas Board Test", C_TEXT_A, C_NONE);
  char t[48], *p = t;
  p = str_cpy(p, board); p = str_cpy(p, "  fw "); p = str_cpy(p, fw);
  scr_text(COLS - 1 - (int) (p - t), 5, t, C_DIM, C_NONE);
  scr_uline(1, 6, COLS - 2, C_LINE);
  for (int i = 0; i < NGROUPS; i++) draw_group(i);
  scr_text(1, ROWS - 1, "Arrows: select  Enter: open  R: run all  Ctrl+Alt+Backspace: exit", C_FOOT, C_NONE);
}

static void draw_clock(void) {
  uint8_t tm[8];
  uint16_t n;
  if (rpc_call(RPC_SYS, 1, 0, 0, tm, sizeof(tm), &n, 500) != RPC_OK || n < 7) return;
  char t[12], *p = t;
  p = fmt_u2(p, tm[4]); *p++ = ':'; p = fmt_u2(p, tm[5]); *p++ = ':'; fmt_u2(p, tm[6]);
  scr_big(COLS - 17, 0, t, C_TEXT);
  p = t;
  p = fmt_u2(p, tm[3]); *p++ = '/'; p = fmt_u2(p, tm[2]); *p++ = '/'; fmt_u2(p, (uint32_t) (tm[0] | tm[1] << 8) % 100);
  scr_big(COLS - 17, 2, t, C_TEXT);
}

// the status line: the frames, the last keyboard report, the mouse
static void draw_status(uint32_t kbd0, uint32_t kbd1, int mx, int my, uint32_t btn) {
  char t[80], *p = t;
  p = str_cpy(p, "frame "); p = fmt_u(p, R_FRAMES);
  p = str_cpy(p, "  keys "); p = fmt_hex(p, kbd0, 8); *p++ = ' '; p = fmt_hex(p, kbd1, 6);
  p = str_cpy(p, "  mouse "); p = fmt_u(p, (uint32_t) mx); *p++ = ','; p = fmt_u(p, (uint32_t) my);
  *p++ = ' '; p = fmt_hex(p, btn, 2);
  scr_text_n(1, ROWS - 2, t, COLS - 2, C_FOOT, C_NONE);
}

static int key_pressed(uint32_t kbd0, uint32_t kbd1, uint8_t code) {
  return ((kbd0 >> 8) & 0xFF) == code || ((kbd0 >> 16) & 0xFF) == code || (kbd0 >> 24) == code ||
         (kbd1 & 0xFF) == code || ((kbd1 >> 8) & 0xFF) == code || ((kbd1 >> 16) & 0xFF) == code;
}

int main(void) {
  // the firmware says HELLO after it took the core; without RPC the screen shows the Classic colours
  uint32_t t0 = R_MS;
  while (!rpc_enabled() && R_MS - t0 < 3000) ;
  char board[8] = "?", fw[9] = "?";
  if (rpc_enabled()) {
    load_look();
    uint16_t n;
    if (rpc_call(RPC_SYS, 0, 0, 0, buf, sizeof(buf), &n, 1000) == RPC_OK && n >= 9) {
      for (int i = 0; i < 8; i++) fw[i] = buf[i] ? (char) buf[i] : 0;
      fw[8] = 0;
      str_cpy(board, buf[8] == 1 ? "Go" : buf[8] == 2 ? "Mini" : buf[8] == 3 ? "Mini G" : "?");
    }
  }
  scr_init();
  draw_main(board, fw);
  if (rpc_enabled()) draw_clock();
  dbg("bt: drawn\n");

  int mx = SCR_W / 2, my = SCR_H / 2;
  uint32_t last_clock = 0, last_kbdn = R_KBDN, kbd0 = 0, kbd1 = 0, btn = 0;
  for (;;) {
    uint32_t m = R_MOUSE;
    int dx = (int32_t) (m << 20) >> 20, dy = (int32_t) (m << 8) >> 20;
    if (dx || dy || (m >> 24) != btn) {
      mx += dx; my += dy;
      mx = mx < 0 ? 0 : mx >= SCR_W ? SCR_W - 1 : mx;
      my = my < 0 ? 0 : my >= SCR_H ? SCR_H - 1 : my;
      btn = m >> 24;
      R_CURSOR = 0x80000000u | (uint32_t) my << 16 | (uint32_t) mx;
    }
    if (R_KBDN != last_kbdn) {
      last_kbdn = R_KBDN;
      uint32_t k0 = R_KBD0, k1 = R_KBD1;
      // the arrows move the selection (USB usage codes: Right 4F, Left 50, Down 51, Up 52)
      int old = sel;
      if (key_pressed(k0, k1, 0x51) && !key_pressed(kbd0, kbd1, 0x51)) sel = (sel + 1) % NGROUPS;
      if (key_pressed(k0, k1, 0x52) && !key_pressed(kbd0, kbd1, 0x52)) sel = (sel + NGROUPS - 1) % NGROUPS;
      if ((key_pressed(k0, k1, 0x4F) && !key_pressed(kbd0, kbd1, 0x4F)) ||
          (key_pressed(k0, k1, 0x50) && !key_pressed(kbd0, kbd1, 0x50))) sel = (sel + 7) % 14;
      if (sel != old) { draw_group(old); draw_group(sel); }
      kbd0 = k0; kbd1 = k1;
    }
    if (R_MS - last_clock >= 500) {
      last_clock = R_MS;
      if (rpc_enabled()) draw_clock();
      draw_status(kbd0, kbd1, mx, my, btn);
    }
  }
}
