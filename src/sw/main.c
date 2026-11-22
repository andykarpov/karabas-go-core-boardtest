// The program of the board test CPU (rtl/soc/bt_soc.v): the main screen of the groups (docs/img/f0_mock_main.png),
// a page per group (the left list and the check, docs/img/f0_mock_sram.png), the keys and the mouse. The checks are in
// tests.c; the host build (host/) runs the same code with a fake board for the frames of the screens.
#include "app.h"
#include "mmio.h"
#include "rpc.h"
#include "scr.h"
#include "util.h"

static uint8_t buf[RPC_MAX];

static void dbg(const char *s) { while (*s) R_DBG = (uint8_t) *s++; }
uint32_t now_ms(void) { return R_MS; }

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

// ---------------------------------------------------------------- the screens

#define PER_COL 9
#define COL2 40
static int sel;          // the group chosen
static int open_g = -1;  // the page shown, -1 the main screen
char board_name[8] = "?", fw_build[9] = "?";

static const char *st_text(int st) {
  switch (st) {
    case ST_RUN: return "RUN ";
    case ST_ASK: return "Y/N?";
    case ST_PASS: return "PASS";
    case ST_FAIL: return "FAIL";
    case ST_NA: case ST_NA_FW: return "N/A ";
    default: return " -- ";
  }
}
static int st_color(int st) {
  switch (st) {
    case ST_RUN: case ST_ASK: return C_RUN;
    case ST_PASS: return C_PASS;
    case ST_FAIL: return C_FAIL;
    default: return C_IDLE;
  }
}

static void draw_group(int i) {
  group_t *g = &groups[i];
  int col = i < PER_COL ? 1 : COL2, row = 8 + 2 * (i % PER_COL);
  scr_button(col, row, 17, g->name, i == sel ? C_ACTIVE : C_BUTTON, C_TEXT_A, 1, -2);
  scr_pill(col + 18, row, st_text(g->status), st_color(g->status));
  scr_text_n(col + 25, row, g->info[0] ? g->info : "not run", COL2 - 2 - 25 + (col == 1 ? 0 : 1), C_DIM, C_NONE);
}

static void draw_clock(void);
static void draw_header(void) {
  scr_clear();
  scr_logo(0, 0, C_LOGO);
  draw_clock();
}

static void draw_main(void) {
  draw_header();
  scr_text(1, 5, "Karabas Board Test", C_TEXT_A, C_NONE);
  char t[48], *p = t;
  p = str_cpy(p, board_name); p = str_cpy(p, "  fw "); p = str_cpy(p, fw_build);
  scr_text(COLS - 1 - (int) (p - t), 5, t, C_DIM, C_NONE);
  scr_uline(1, 6, COLS - 2, C_LINE);
  for (int i = 0; i < ngroups; i++) draw_group(i);
  scr_text(1, ROWS - 1, "Arrows: select  Enter: open  Ctrl+Alt+Backspace: exit", C_FOOT, C_NONE);
}

// the page: a window of 10 groups around the open one on the left, the check on the right
static void draw_page_list(void) {
  int first = open_g - 4;
  if (first > ngroups - 10) first = ngroups - 10;
  if (first < 0) first = 0;
  for (int k = 0; k < 10; k++) {
    int i = first + k;
    if (i >= ngroups) break;
    group_t *g = &groups[i];
    int dot = g->status == ST_PASS ? C_PASS : g->status == ST_FAIL ? C_FAIL :
              (g->status == ST_RUN || g->status == ST_ASK) ? C_RUN : -1;
    scr_button(1, 5 + 2 * k, 17, g->name, i == open_g ? C_ACTIVE : C_BUTTON, C_TEXT_A, 0, dot);
  }
}

void page_title(group_t *g) {
  scr_fill(PX, 5, COLS - PX - 1, C_NONE);
  int c = scr_text(PX, 5, g->name, C_TEXT_A, C_NONE);
  scr_pill(c + 2, 5, st_text(g->status), st_color(g->status));
  scr_uline(PX, 6, COLS - PX - 1, C_LINE);
}

void page_row(int r, const char *label, const char *value, int color) {
  scr_text_n(PX, PROW + r, label, 10, C_DIM, C_NONE);
  scr_text_n(PX + 10, PROW + r, value, COLS - PX - 11, color, C_NONE);
}

void page_hint(const char *s) { scr_text_n(1, ROWS - 2, s, COLS - 2, C_FOOT, C_NONE); }

void set_status(group_t *g, int st, const char *info) {
  int changed = g->status != st;
  g->status = (uint8_t) st;
  if (info) str_copy_n(g->info, info, sizeof(g->info));
  if (open_g >= 0 && changed) { page_title(g); draw_page_list(); }
}

static void open_page(int i) {
  open_g = i;
  draw_header();
  draw_page_list();
  page_title(&groups[i]);
  scr_text(1, ROWS - 1, "Esc: back to the list  Ctrl+Alt+Backspace: exit", C_FOOT, C_NONE);
  if (groups[i].open) groups[i].open(&groups[i]);
}

void page_redraw(void) {   // the page again (the video pictures took the screen)
  open_page(open_g);
}

static void close_page(void) {
  group_t *g = &groups[open_g];
  if (g->close) g->close(g);
  open_g = -1;
  draw_main();
}

static void draw_clock(void) {
  uint8_t tm[8];
  uint16_t n;
  if (!rpc_enabled() || rpc_call(RPC_SYS, 1, 0, 0, tm, sizeof(tm), &n, 500) != RPC_OK || n < 7) return;
  char t[12], *p = t;
  p = fmt_u2(p, tm[4]); *p++ = ':'; p = fmt_u2(p, tm[5]); *p++ = ':'; fmt_u2(p, tm[6]);
  scr_big(COLS - 17, 0, t, C_TEXT);
  p = t;
  p = fmt_u2(p, tm[3]); *p++ = '/'; p = fmt_u2(p, tm[2]); *p++ = '/'; fmt_u2(p, (uint32_t) (tm[0] | tm[1] << 8) % 100);
  scr_big(COLS - 17, 2, t, C_TEXT);
}

// ---------------------------------------------------------------- the keys: the presses of the keyboard reports

static uint32_t kbd0, kbd1, kbdn;
static uint8_t keyq[8];
static int keyq_n;

static int in_report(uint32_t k0, uint32_t k1, int code) {
  uint32_t c = (uint32_t) code;
  return ((k0 >> 8) & 0xFF) == c || ((k0 >> 16) & 0xFF) == c || (k0 >> 24) == c ||
         (k1 & 0xFF) == c || ((k1 >> 8) & 0xFF) == c || ((k1 >> 16) & 0xFF) == c;
}
int key_held(int code) { return in_report(kbd0, kbd1, code); }

static void poll_keys(void) {
  if (R_KBDN == kbdn) return;
  kbdn = R_KBDN;
  uint32_t k0 = R_KBD0, k1 = R_KBD1;
  uint32_t codes[6] = { (k0 >> 8) & 0xFF, (k0 >> 16) & 0xFF, k0 >> 24, k1 & 0xFF, (k1 >> 8) & 0xFF, (k1 >> 16) & 0xFF };
  for (int i = 0; i < 6; i++)
    if (codes[i] > 3 && !in_report(kbd0, kbd1, (int) codes[i]) && keyq_n < 8) keyq[keyq_n++] = (uint8_t) codes[i];
  kbd0 = k0; kbd1 = k1;
}

static int next_key(void) {
  if (!keyq_n) return 0;
  int k = keyq[0];
  for (int i = 1; i < keyq_n; i++) keyq[i - 1] = keyq[i];
  keyq_n--;
  return k;
}

// ---------------------------------------------------------------- the loop

static int mx = SCR_W / 2, my = SCR_H / 2;
static uint32_t mbtn, last_clock;
// the last mouse report for the pages (R_MOUSE clears on read: only poll_mouse reads it)
int ms_dx, ms_dy;
uint32_t ms_btn, ms_seq;

void app_init(void) {
  uint32_t t0 = R_MS;
  while (!rpc_enabled() && R_MS - t0 < 3000) ;
  if (rpc_enabled()) {
    load_look();
    uint16_t n;
    if (rpc_call(RPC_SYS, 0, 0, 0, buf, sizeof(buf), &n, 1000) == RPC_OK && n >= 9) {
      for (int i = 0; i < 8; i++) fw_build[i] = (char) buf[i];
      fw_build[8] = 0;
      str_cpy(board_name, buf[8] == 1 ? "Go" : buf[8] == 2 ? "Mini" : buf[8] == 3 ? "Mini G" : "?");
    }
  }
  scr_init();
  draw_main();
  draw_clock();
  kbdn = R_KBDN;
  dbg("bt: drawn\n");
}

// the mouse: the cursor; a click on a group of the main screen opens it
static void poll_mouse(void) {
  uint32_t m = R_MOUSE;
  int dx = (int32_t) (m << 20) >> 20, dy = (int32_t) (m << 8) >> 20;
  uint32_t b = m >> 24;
  if (!dx && !dy && b == mbtn) return;
  ms_dx = dx; ms_dy = dy; ms_btn = b; ms_seq++;
  mx += dx; my += dy;
  mx = mx < 0 ? 0 : mx >= SCR_W ? SCR_W - 1 : mx;
  my = my < 0 ? 0 : my >= SCR_H ? SCR_H - 1 : my;
  R_CURSOR = 0x80000000u | (uint32_t) my << 16 | (uint32_t) mx;
  if ((b & 1) && !(mbtn & 1) && open_g < 0) {
    int c = mx / CELL_W, r = my / CELL_H;
    if (r >= 8 && (r - 8) % 2 == 0 && (r - 8) / 2 < PER_COL) {
      int i = (r - 8) / 2 + (c >= COL2 ? PER_COL : 0);
      if (i < ngroups && (c < 18 || (c >= COL2 && c < COL2 + 17))) { sel = i; open_page(i); }
    }
  }
  mbtn = b;
}

void app_open(int i) { sel = i; open_page(i); }   // the host build

void app_step(void) {
  poll_keys();
  poll_mouse();
  int k = next_key();
  if (open_g < 0) {
    int old = sel;
    if (k == K_DOWN) sel = (sel + 1) % ngroups;
    else if (k == K_UP) sel = (sel + ngroups - 1) % ngroups;
    else if (k == K_RIGHT || k == K_LEFT) sel = sel < PER_COL ? (sel + PER_COL < ngroups ? sel + PER_COL : ngroups - 1) : sel - PER_COL;
    else if (k == K_ENTER) { open_page(sel); return; }
    if (sel != old) { draw_group(old); draw_group(sel); }
  } else {
    group_t *g = &groups[open_g];
    // Esc leaves the page, except on a page that tests the keys (v[7] = 1: it leaves on a held Esc itself)
    if (k == K_ESC && g->v[7] != 1) { close_page(); return; }
    if (k && g->key) g->key(g, k);
    if (open_g >= 0 && g->tick) g->tick(g);
    if (open_g >= 0 && g->v[7] == 2) { g->v[7] = 1; close_page(); return; }   // the page asked to be left
  }
  if (R_MS - last_clock >= 500) {
    last_clock = R_MS;
    draw_clock();
  }
}

#ifndef HOST
int main(void) {
  app_init();
  for (;;) app_step();
}
#endif
