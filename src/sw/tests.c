// The checks of the groups (docs/PLAN.md "Проверки", Ф1): each group has a page (open / tick / key / close, app.h).
// A result is set automatically where the board can measure it, else the operator answers Y / N; a part that is
// not on the board (the LED matrix add-on) is N/A, one the firmware cannot serve yet is N/A (firmware).
#include "app.h"
#include "io.h"
#include "mmio.h"
#include "rpc.h"
#include "scr.h"
#include "util.h"

void str_copy_n(char *d, const char *s, int n) {
  int i = 0;
  for (; s[i] && i < n - 1; i++) d[i] = s[i];
  d[i] = 0;
}

static char t[80];
static uint8_t rbuf[32];

// RPC SYS I2C_SCAN (firmware PR #166): 16 bytes, a bit per address; RPC_UNKNOWN on an older firmware
static int i2c_scan(uint8_t *bits) {
  uint16_t n;
  int st = rpc_enabled() ? rpc_call(RPC_SYS, 3, 0, 0, bits, 16, &n, 500) : RPC_TIMEOUT;
  if (st == RPC_OK && n < 16) st = RPC_IO;
  return st;
}
static int i2c_has(const uint8_t *bits, int a) { return (bits[a >> 3] >> (a & 7)) & 1; }

// RPC SYS LED (firmware PR #166): mask, on
static int led_set(int mask, int on) {
  uint8_t b[2] = { (uint8_t) mask, (uint8_t) on };
  return rpc_enabled() ? rpc_call(RPC_SYS, 4, b, 2, 0, 0, 0, 500) : RPC_TIMEOUT;
}

static int time_get(uint8_t *tm) {
  uint16_t n;
  int st = rpc_enabled() ? rpc_call(RPC_SYS, 1, 0, 0, tm, 8, &n, 500) : RPC_TIMEOUT;
  return st == RPC_OK && n >= 7 ? RPC_OK : st == RPC_OK ? RPC_IO : st;
}

static char *fmt_time(char *p, const uint8_t *tm) {
  p = fmt_u2(p, tm[4]); *p++ = ':'; p = fmt_u2(p, tm[5]); *p++ = ':'; p = fmt_u2(p, tm[6]); *p++ = ' ';
  p = fmt_u2(p, tm[3]); *p++ = '/'; p = fmt_u2(p, tm[2]); *p++ = '/'; p = fmt_u(p, (uint32_t) (tm[0] | tm[1] << 8));
  return p;
}

// a meter of 0..32767 in w cells with its value after it
static void meter_row(int r, const char *label, uint32_t v, int w) {
  scr_text_n(PX, PROW + r, label, 10, C_DIM, C_NONE);
  scr_meter(PX + 10, PROW + r, w, (int) (v * 1000 / 32768), v > 30000 ? C_FAIL : C_ACTIVE);
  char *p = t; p = fmt_u(p, v); str_cpy(p, "     ");
  scr_text_n(PX + 11 + w, PROW + r, t, 7, C_TEXT, C_NONE);
}

static void ask(group_t *g, const char *question) {
  page_row(14, "", question, C_TEXT_A);
  page_row(15, "", "Y: yes   N: no", C_DIM);
  if (g->status != ST_ASK) set_status(g, ST_ASK, 0);
}

// ---------------------------------------------------------------- FPGA / MCU

extern char board_name[8], fw_build[9];

static void fpga_open(group_t *g) {
  uint32_t hi = IO_DNA_HI, lo = IO_DNA_LO;
  char *p = t;
  if (hi >> 31) { p = str_cpy(p, "0x"); p = fmt_hex(p, hi & 0x1FFFFFF, 7); fmt_hex(p, lo, 8); }
  else str_cpy(t, "not read");
  page_row(0, "DNA", t, (hi >> 31) ? C_TEXT : C_FAIL);
  page_row(1, "Board", board_name, C_TEXT);
  page_row(2, "Firmware", fw_build, C_TEXT);
  page_row(3, "MCU link", rpc_enabled() ? "link v2, RPC up" : "no RPC (old firmware?)", rpc_enabled() ? C_TEXT : C_FAIL);
  page_row(4, "CPU", "VexRiscv rv32im 75 MHz, 32 KB block RAM", C_TEXT);
  page_row(5, "Video", (IO_BTN >> 8) & 1 ? "DVI (no audio), 800 x 600 60 Hz" : "HDMI, 800 x 600 60 Hz", C_TEXT);
  int ok = (hi >> 31) && rpc_enabled();
  p = str_cpy(t, "DNA "); fmt_hex(p, lo, 8);
  set_status(g, ok ? ST_PASS : ST_FAIL, ok ? t : "no DNA / RPC");
}

// ---------------------------------------------------------------- RTC / EEPROM

static uint8_t tm0[8], tm1[8];

static void rtc_open(group_t *g) {
  g->v[0] = 0;
  g->v[1] = (int32_t) now_ms();
  int st = time_get(tm0);
  page_row(0, "RTC", st == RPC_OK ? "DS3231 answers" : st == RPC_NOT_FOUND ? "no clock" : "no answer", st == RPC_OK ? C_TEXT : C_FAIL);
  g->v[2] = st == RPC_OK;
  if (st == RPC_OK) { fmt_time(t, tm0); page_row(1, "Time", t, C_TEXT); }
  page_row(2, "Ticking", "...", C_DIM);
  // the I2C devices of the MCU
  st = i2c_scan(rbuf);
  g->v[3] = st == RPC_OK ? (i2c_has(rbuf, 0x50) ? 1 : 0) : -1;   // the EEPROM: 1 found, 0 not, -1 firmware
  page_row(4, "EEPROM", g->v[3] > 0 ? "24C08 at 0x50" : g->v[3] == 0 ? "not found at 0x50" :
           "N/A: the firmware has no I2C_SCAN (PR #166)", g->v[3] == 0 ? C_FAIL : g->v[3] > 0 ? C_TEXT : C_DIM);
  if (st == RPC_OK) {
    char *p = t;
    for (int a = 8; a < 0x78 && p < t + 60; a++) if (i2c_has(rbuf, a)) { p = fmt_hex(p, (uint32_t) a, 2); *p++ = ' '; }
    *p = 0;
    page_row(5, "I2C", t[0] ? t : "nothing answers", C_TEXT);
  }
  set_status(g, ST_RUN, 0);
}

static void rtc_tick(group_t *g) {
  if (g->v[0] || now_ms() - (uint32_t) g->v[1] < 1500) return;
  g->v[0] = 1;
  int ok = g->v[2] && time_get(tm1) == RPC_OK;
  int ds = ok ? ((tm1[6] + 60 - tm0[6]) % 60) : 0;
  int year = tm1[0] | tm1[1] << 8;
  int sane = ok && year >= 2024 && year <= 2099 && tm1[2] >= 1 && tm1[2] <= 12 && tm1[3] >= 1 && tm1[3] <= 31;
  int tick = ds >= 1 && ds <= 3;
  page_row(2, "Ticking", !ok ? "-" : tick ? "yes" : "no: the seconds stand still", tick ? C_TEXT : C_FAIL);
  if (ok) { fmt_time(t, tm1); page_row(1, "Time", t, sane ? C_TEXT : C_FAIL); }
  if (ok && !sane) page_row(3, "", "the date is not set (or the battery is flat)", C_FAIL);
  int pass = ok && tick && sane && g->v[3] != 0;
  set_status(g, pass ? ST_PASS : ST_FAIL, !ok ? "no RTC" : !tick ? "RTC stopped" : !sane ? "date not set" :
             g->v[3] == 0 ? "no EEPROM" : g->v[3] < 0 ? "RTC, EE n/a" : "RTC, 24C08");
}

// ---------------------------------------------------------------- Buttons / LEDs

static void btn_draw(group_t *g) {
  uint32_t b = IO_BTN;
  for (int i = 0; i < 2; i++) {
    int held = (b >> i) & 1, seen = (g->v[0] >> i) & 1, done = (g->v[1] >> i) & 1;
    char *p = str_cpy(t, held ? "pressed " : "released");
    str_cpy(p, done ? "   ok" : seen ? "   release it" : "   press it");
    page_row(i, i ? "BTN 2" : "BTN 1", t, done ? C_PASS : C_TEXT);
  }
}

static void btn_open(group_t *g) {
  g->v[0] = g->v[1] = 0;
  g->v[2] = led_set(3, 1) == RPC_OK ? 1 : -1;   // LED 1 on, LED 2 off; -1: the firmware has no LED call
  g->v[3] = 0;                                    // the LED answers: bit 0 LED 1 yes, bit 1 LED 2 yes, bit 2 a no
  btn_draw(g);
  page_hint("Press and release BTN 1 and BTN 2 on the front panel");
  if (g->v[2] < 0) page_row(3, "LEDs", "N/A: the firmware has no LED call (PR #166)", C_DIM);
  else { page_row(3, "LEDs", "LED 1 on, LED 2 off", C_TEXT); ask(g, "Is LED 1 on and LED 2 off?"); }
  if (g->v[2] < 0) set_status(g, ST_RUN, 0);
}

static void btn_result(group_t *g) {
  int btn_ok = g->v[1] == 3, led_ok = g->v[2] < 0 || g->v[3] == 3;
  if (g->v[3] & 4) set_status(g, ST_FAIL, (g->v[3] & 1) ? "LED 2 bad" : "LED 1 bad");
  else if (btn_ok && led_ok && g->v[2] != 1 && g->v[2] != 2)
    set_status(g, ST_PASS, g->v[2] < 0 ? "BTN ok, LED n/a" : "BTN 1, 2, LEDs");
}

static void btn_tick(group_t *g) {
  uint32_t b = IO_BTN & 3;
  int was = g->v[1];
  g->v[0] |= (int32_t) b;
  g->v[1] |= g->v[0] & ~(int32_t) b;
  static uint32_t last;
  if (b != last || g->v[1] != was) { last = b; btn_draw(g); btn_result(g); }
}

static void btn_key(group_t *g, int k) {
  if (g->v[2] != 1 && g->v[2] != 2) return;
  if (k != K_Y && k != K_N) return;
  if (k == K_N) g->v[3] |= 4;
  else g->v[3] |= g->v[2];      // 1 or 2: the LED asked
  if (g->v[2] == 1 && k == K_Y) {
    g->v[2] = 2;
    led_set(3, 2);
    page_row(3, "LEDs", "LED 1 off, LED 2 on", C_TEXT);
    ask(g, "Is LED 2 on and LED 1 off?");
    return;
  }
  g->v[2] = 3;
  led_set(0, 0);
  page_row(3, "LEDs", (g->v[3] & 4) ? "the operator saw a wrong LED" : "both answered yes", (g->v[3] & 4) ? C_FAIL : C_PASS);
  page_row(14, "", "", C_TEXT); page_row(15, "", "", C_TEXT);
  set_status(g, ST_RUN, 0);
  btn_result(g);
}

static void btn_close(group_t *g) { (void) g; led_set(0, 0); }

// ---------------------------------------------------------------- LED matrix

static void mx_fill(int pic) {
  // pic 0 red, 1 green, 2 yellow, 3 a column walking (yellow)
  for (int y = 0; y < 8; y++) {
    uint8_t r = pic == 0 || pic == 2 ? 0xFF : 0, gr = pic == 1 || pic == 2 ? 0xFF : 0;
    IO_MATRIX = MX_BITPLAN(y, r);
    IO_MATRIX = MX_BITPLAN(8 + y, gr);
    IO_MATRIX = MX_BITPLAN(16 + y, r);
    IO_MATRIX = MX_BITPLAN(24 + y, gr);
  }
  IO_MATRIX = MX_CTL(0x02);     // update, custom draw
}

static void mx_open(group_t *g) {
  int st = i2c_scan(rbuf);
  g->v[0] = st == RPC_OK ? (i2c_has(rbuf, 0x70) + i2c_has(rbuf, 0x71)) : -1;
  if (g->v[0] == 0) {
    page_row(0, "HT16K33", "none at 0x70 / 0x71: the add-on is not fitted", C_TEXT);
    set_status(g, ST_NA, "not fitted");
    return;
  }
  page_row(0, "HT16K33", g->v[0] == 2 ? "0x70 and 0x71 answer" : g->v[0] == 1 ? "only one of 0x70 / 0x71 answers" :
           "unknown (the firmware has no I2C_SCAN)", g->v[0] == 1 ? C_FAIL : C_TEXT);
  page_row(1, "Pictures", "red, green, yellow, one second each", C_TEXT);
  g->v[1] = 0; g->v[2] = (int32_t) now_ms(); g->v[3] = 0;
  mx_fill(0);
  page_hint("The matrix buttons are shown below; they are not needed for the result");
  ask(g, "Do both matrices show red, green and yellow?");
}

static void mx_tick(group_t *g) {
  if (g->v[0] == 0) return;
  if (now_ms() - (uint32_t) g->v[2] >= 1000) { g->v[2] = (int32_t) now_ms(); g->v[1] = (g->v[1] + 1) % 3; mx_fill(g->v[1]); }
  uint32_t b = (IO_BTN >> 2) & 15;
  if ((int32_t) b != g->v[3]) {
    g->v[3] = (int32_t) b;
    char *p = t;
    for (int i = 0; i < 4; i++) { p = str_cpy(p, (b >> i) & 1 ? "[X] " : "[ ] "); }
    page_row(3, "Buttons", t, C_TEXT);
  }
}

static void mx_key(group_t *g, int k) {
  if (g->status != ST_ASK || (k != K_Y && k != K_N)) return;
  page_row(14, "", "", C_TEXT); page_row(15, "", "", C_TEXT);
  set_status(g, k == K_Y && g->v[0] != 1 ? ST_PASS : ST_FAIL, k == K_Y ? (g->v[0] == 1 ? "one chip" : "colours ok") : "wrong colours");
}

static void mx_close(group_t *g) {
  (void) g;
  IO_MATRIX = MX_CTL(0x09);     // clear, the audio meters again
}

// ---------------------------------------------------------------- Audio

static void aud_set(int ch) {   // 0 left, 1 right: 1 kHz sine, the ADC off (a DAC -> line in cable loops back)
  IO_INC_L = AUD_INC(1000); IO_INC_R = AUD_INC(1000);
  IO_AUD_CTL = (ch ? AUD_GEN_R : AUD_GEN_L) | AUD_SINE | AUD_VOL(96);
}

static void aud_open(group_t *g) {
  g->v[0] = 0; g->v[1] = (int32_t) now_ms(); g->v[2] = (int32_t) IO_ADC_ZC_L; g->v[3] = (int32_t) IO_ADC_ZC_R;
  g->v[4] = 0;
  aud_set(0);
  page_row(0, "Tone", "1 kHz sine: left 1.5 s, then right 1.5 s", C_TEXT);
  page_row(1, "Output", (IO_BTN >> 8) & 1 ? "the DAC (DVI: no audio on HDMI)" : "the DAC and HDMI", C_TEXT);
  page_hint("A cable from the DAC out to the line in: the ADC measures the tones too");
  ask(g, "Did you hear it on the left, then on the right?");
}

static void aud_tick(group_t *g) {
  uint32_t n = now_ms();
  if (n - (uint32_t) g->v[1] < 1500) return;
  // a phase is over: the frequency the ADC saw on each side (rising zero crossings in 1.5 s)
  uint32_t zl = IO_ADC_ZC_L, zr = IO_ADC_ZC_R, pk = IO_ADC_PEAK;
  uint32_t fl = (zl - (uint32_t) g->v[2]) * 2 / 3, fr = (zr - (uint32_t) g->v[3]) * 2 / 3;
  int side = g->v[0];
  uint32_t f = side ? fr : fl, other = side ? fl : fr;
  int loop = f > 950 && f < 1050 && other < 100;
  if (loop) g->v[4] |= 1 << side;
  char *p = t;
  p = str_cpy(p, side ? "right " : "left  "); p = fmt_u(p, f); p = str_cpy(p, " Hz, other side "); p = fmt_u(p, other);
  str_cpy(p, " Hz");
  page_row(3 + side, side ? "ADC R" : "ADC L", t, loop ? C_PASS : C_TEXT);
  meter_row(5, "ADC peak", side ? pk >> 16 : pk & 0xFFFF, 30);
  if (g->v[4] == 3) page_row(7, "Loopback", "both channels come back at 1 kHz", C_PASS);
  g->v[0] ^= 1; g->v[1] = (int32_t) n; g->v[2] = (int32_t) zl; g->v[3] = (int32_t) zr;
  aud_set(g->v[0]);
}

static void aud_key(group_t *g, int k) {
  if (g->status != ST_ASK || (k != K_Y && k != K_N)) return;
  page_row(14, "", "", C_TEXT); page_row(15, "", "", C_TEXT);
  set_status(g, k == K_Y ? ST_PASS : ST_FAIL, k == K_N ? "not heard" : g->v[4] == 3 ? "L, R, loopback" : "L, R heard");
}

static void aud_close(group_t *g) { (void) g; IO_AUD_CTL = AUD_ADC; }

// ---------------------------------------------------------------- Tape

static void tape_open(group_t *g) {
  // TAPE_OUT: a 1 kHz square of generator L (silent: volume 0), the tape monitor hears the input
  IO_INC_L = AUD_INC(1000);
  IO_AUD_CTL = AUD_GEN_L | AUD_TAPE_OUT | AUD_MON(1) | AUD_VOL(0);
  g->v[0] = (int32_t) now_ms(); g->v[1] = (int32_t) (IO_TAPE >> 8);
  page_row(0, "TAPE OUT", "a 1 kHz square", C_TEXT);
  page_row(1, "TAPE IN", "...", C_DIM);
  page_hint("Connect TAPE OUT to TAPE IN: the input then counts 1000 edges a second (heard too)");
  set_status(g, ST_RUN, 0);
}

static void tape_tick(group_t *g) {
  uint32_t n = now_ms();
  if (n - (uint32_t) g->v[0] < 1000) return;
  uint32_t tp = IO_TAPE, e = (tp >> 8) - (uint32_t) g->v[1];
  g->v[0] = (int32_t) n; g->v[1] = (int32_t) (tp >> 8);
  char *p = t;
  p = str_cpy(p, tp & 1 ? "high, " : "low, "); p = fmt_u(p, e); str_cpy(p, " edges a second");
  int loop = e > 950 && e < 1050;
  page_row(1, "TAPE IN", t, loop ? C_PASS : C_TEXT);
  page_row(3, "Loopback", loop ? "TAPE OUT comes back on TAPE IN" : e ? "a signal, not the 1 kHz of TAPE OUT" :
           "no signal: connect TAPE OUT to TAPE IN", loop ? C_PASS : C_DIM);
  if (loop && g->status != ST_PASS) set_status(g, ST_PASS, "loopback 1 kHz");
}

static void tape_close(group_t *g) { (void) g; IO_AUD_CTL = AUD_ADC; }

// ---------------------------------------------------------------- MIDI

static const uint8_t scale[8] = { 60, 62, 64, 65, 67, 69, 71, 72 };

static void midi_open(group_t *g) {
  IO_AUD_CTL = AUD_ADC;     // the SAM2695 is heard through the ADC
  g->v[0] = -1; g->v[1] = (int32_t) now_ms(); g->v[2] = 0; g->v[3] = 0;
  (void) IO_ADC_PEAK;
  page_row(0, "Synth", "the SAM2695 on MIDI_TX (31250 baud), into the ADC", C_TEXT);
  page_row(1, "Notes", "a scale of C, piano", C_TEXT);
  page_hint("R: the scale again");
  set_status(g, ST_RUN, 0);
}

static void midi_tick(group_t *g) {
  uint32_t n = now_ms();
  if (g->v[0] >= 8 || n - (uint32_t) g->v[1] < 250) return;
  uint32_t pk = IO_ADC_PEAK, p = (pk & 0xFFFF) > (pk >> 16) ? pk & 0xFFFF : pk >> 16;
  if (g->v[0] < 0) g->v[2] = (int32_t) p;                       // the silence before the notes
  else {
    IO_MIDI = 0x80; IO_MIDI = scale[g->v[0]]; IO_MIDI = 0;      // note off
    if ((int32_t) p > g->v[3]) g->v[3] = (int32_t) p;
  }
  g->v[0]++;
  g->v[1] = (int32_t) n;
  if (g->v[0] < 8) { IO_MIDI = 0x90; IO_MIDI = scale[g->v[0]]; IO_MIDI = 100; return; }
  // the scale is over: the ADC saw it?
  char *q = t;
  q = str_cpy(q, "silence "); q = fmt_u(q, (uint32_t) g->v[2]); q = str_cpy(q, ", notes "); fmt_u(q, (uint32_t) g->v[3]);
  int heard = g->v[3] > 1000 && g->v[3] > 4 * g->v[2];
  page_row(3, "ADC peak", t, heard ? C_PASS : C_TEXT);
  if (heard) { page_row(4, "", "the synth is heard on the ADC", C_PASS); set_status(g, ST_PASS, "SAM2695 on ADC"); }
  else ask(g, "Did you hear the scale?");
}

static void midi_key(group_t *g, int k) {
  if (k == K_R) { midi_open(g); return; }
  if (g->status != ST_ASK || (k != K_Y && k != K_N)) return;
  page_row(14, "", "", C_TEXT); page_row(15, "", "", C_TEXT);
  set_status(g, k == K_Y ? ST_PASS : ST_FAIL, k == K_Y ? "scale heard" : "no sound");
}

static void midi_close(group_t *g) {
  (void) g;
  IO_MIDI = 0xB0; IO_MIDI = 123; IO_MIDI = 0;   // all notes off
}

// ---------------------------------------------------------------- Video

void page_redraw(void);
extern int fullscreen;

static const uint32_t bars[8] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000 };
static const uint32_t ramps[4] = { 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF };

static void vid_picture(int pic) {
  scr_clear();
  if (pic == 0) {
    for (int i = 0; i < 8; i++) R_PAL(1 + i) = 0xF0000000u | bars[i];
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) scr_put(c, r, T_SOLID, 1 + c / 10, 0);
  } else {
    // a ramp: the gradient of menu_compose from black at the top to the colour at 3/4 of the height
    R_GRAD_TOP = 0; R_GRAD_BOT = ramps[pic - 1];
  }
}

static void vid_open(group_t *g) {
  if (g->v[0] == 2) {           // back from the pictures
    ask(g, "Were the bars and the four ramps right?");
    return;
  }
  g->v[0] = 0;
  page_row(0, "Output", (IO_BTN >> 8) & 1 ? "DVI, 800 x 600 60 Hz" : "HDMI, 800 x 600 60 Hz", C_TEXT);
  page_row(1, "Pictures", "colour bars, then ramps of white, red, green, blue", C_TEXT);
  page_row(3, "", "Enter: the pictures (any key: the next one)", C_TEXT_A);
  set_status(g, ST_RUN, 0);
}

static void vid_key(group_t *g, int k) {
  if (g->status == ST_ASK && (k == K_Y || k == K_N)) {
    page_row(14, "", "", C_TEXT); page_row(15, "", "", C_TEXT);
    g->v[0] = 0;
    set_status(g, k == K_Y ? ST_PASS : ST_FAIL, k == K_Y ? "bars, ramps ok" : "picture wrong");
    return;
  }
  if (g->v[0] == 0 && k == K_ENTER) { g->v[0] = 1; g->v[1] = 0; g->v[7] = 1; fullscreen = 1; vid_picture(0); return; }
  if (g->v[0] == 1) {
    if (++g->v[1] < 5) { vid_picture(g->v[1]); return; }
    g->v[0] = 2; g->v[7] = 0; fullscreen = 0;
    scr_init();                 // the palette and the gradient of the look again
    page_redraw();
  }
}

static void vid_close(group_t *g) { if (g->v[0] == 1) { g->v[0] = 0; fullscreen = 0; scr_init(); } }

// ---------------------------------------------------------------- Input

static const char *key_name(int c, char *b) {
  if (c >= 4 && c <= 29) { b[0] = (char) ('A' + c - 4); b[1] = 0; return b; }
  if (c >= 30 && c <= 39) { b[0] = (char) (c == 39 ? '0' : '1' + c - 30); b[1] = 0; return b; }
  if (c >= 0x3A && c <= 0x45) { char *p = str_cpy(b, "F"); fmt_u(p, (uint32_t) (c - 0x39)); return b; }
  switch (c) {
    case K_ENTER: return "Enter"; case K_ESC: return "Esc"; case K_BKSP: return "Bksp"; case 0x2B: return "Tab";
    case K_SPACE: return "Space"; case K_RIGHT: return "Right"; case K_LEFT: return "Left"; case K_DOWN: return "Down";
    case K_UP: return "Up";
  }
  char *p = str_cpy(b, "0x"); fmt_hex(p, (uint32_t) c, 2);
  return b;
}

static int32_t m_x, m_y, m_w;
extern int ms_dx, ms_dy;
extern uint32_t ms_btn, ms_seq;

static void in_open(group_t *g) {
  for (int i = 0; i < 7; i++) g->v[i] = 0;
  g->v[7] = 1;                  // the page takes Esc: a held Esc leaves it
  g->v[5] = -1;
  g->v[4] = (int32_t) ms_seq;
  g->v[3] = -1;
  m_x = m_y = m_w = 0;
  page_row(0, "Keys", "press keys (up to six at once)", C_DIM);
  page_hint("Hold Esc for a second to leave this page");
  set_status(g, ST_RUN, 0);
}


static void in_tick(group_t *g) {
  static const char *mods[8] = { "LCtrl", "LShift", "LAlt", "LGui", "RCtrl", "RShift", "RAlt", "RGui" };
  uint32_t k0 = R_KBD0, k1 = R_KBD1;
  if ((int32_t) R_KBDN != g->v[5]) {
    g->v[5] = (int32_t) R_KBDN;
    g->v[0] = 1;
    uint32_t codes[6] = { (k0 >> 8) & 0xFF, (k0 >> 16) & 0xFF, k0 >> 24, k1 & 0xFF, (k1 >> 8) & 0xFF, (k1 >> 16) & 0xFF };
    char *p = t, b[8];
    for (int i = 0; i < 6; i++) if (codes[i]) { p = str_cpy(p, key_name((int) codes[i], b)); *p++ = ' '; }
    *p = 0;
    page_row(0, "Keys", t[0] ? t : "-", C_TEXT_A);
    int c = PX + 10;
    for (int i = 0; i < 8; i++) c = scr_text(c, PROW + 1, mods[i], (k0 >> i) & 1 ? C_PASS : C_IDLE, C_NONE) + 1;
    scr_text(PX, PROW + 1, "Modifiers", C_DIM, C_NONE);
  }
  // Esc held a second leaves the page: the keyboard register itself each time (a release does not always come
  // with a new report count)
  {
    uint32_t e0 = R_KBD0, e1 = R_KBD1, c = K_ESC;
    int esc = ((e0 >> 8) & 0xFF) == c || ((e0 >> 16) & 0xFF) == c || (e0 >> 24) == c ||
              (e1 & 0xFF) == c || ((e1 >> 8) & 0xFF) == c || ((e1 >> 16) & 0xFF) == c;
    if (!esc) g->v[6] = 0;
    else if (!g->v[6]) g->v[6] = (int32_t) (now_ms() | 1);
  }
  if (g->v[6] && now_ms() - (uint32_t) g->v[6] >= 1000) {
    g->v[6] = 0;
    set_status(g, g->v[0] && g->v[1] ? ST_PASS : ST_RUN, g->v[0] && g->v[1] ? "kbd, mouse" : 0);
    g->v[7] = 2;               // main.c leaves the page
    return;
  }
  // the mouse: the reports main.c read (ms_*: R_MOUSE clears on read), the wheel here
  int w = (int32_t) (R_WHEEL << 20) >> 20;
  if ((int32_t) ms_seq != g->v[4] || w) {
    g->v[4] = (int32_t) ms_seq;
    g->v[1] = 1;
    m_x += ms_dx; m_y += ms_dy; m_w += w;
    ms_dx = ms_dy = 0;
    uint32_t m = ms_btn << 24;
    g->v[2] = (int32_t) ms_btn;
    char *p = t;
    p = str_cpy(p, "moved "); p = fmt_u(p, (uint32_t) (m_x < 0 ? -m_x : m_x)); *p++ = ','; p = fmt_u(p, (uint32_t) (m_y < 0 ? -m_y : m_y));
    p = str_cpy(p, "  wheel "); p = fmt_u(p, (uint32_t) (m_w < 0 ? -m_w : m_w));
    p = str_cpy(p, "  "); p = str_cpy(p, (m >> 24) & 1 ? "L" : "-"); p = str_cpy(p, (m >> 24) & 4 ? "M" : "-");
    str_cpy(p, (m >> 24) & 2 ? "R" : "-");
    page_row(3, "Mouse", t, C_TEXT);
  }
  uint32_t j = R_JOY;
  if (j != (uint32_t) g->v[3]) {
    g->v[3] = (int32_t) j;
    static const char *jb[13] = { "on", "U", "D", "L", "R", "St", "A", "B", "C", "X", "Y", "Z", "Md" };
    for (int s = 0; s < 2; s++) {
      uint32_t v = s ? j >> 16 : j & 0xFFFF;
      int c = PX + 10;
      for (int i = 0; i < 13; i++) c = scr_text(c, PROW + 5 + s, jb[i], (v >> i) & 1 ? C_PASS : C_IDLE, C_NONE) + 1;
      scr_text(PX, PROW + 5 + s, s ? "Joy R" : "Joy L", C_DIM, C_NONE);
    }
  }
  if (g->v[0] && g->v[1] && g->status != ST_PASS) set_status(g, ST_PASS, "kbd, mouse");
}

// ---------------------------------------------------------------- the groups of later phases

static void later_open(group_t *g) {
  page_row(0, "", g->v[6] ? "N/A: no flash chip is fitted on the boards (its pins drive the DAC of old Minis)" :
           "comes in a later phase (docs/PLAN.md)", C_DIM);
}

group_t groups[] = {
  { "FPGA / MCU", fpga_open, 0, 0, 0, ST_IDLE, "", {0} },
  { "SRAM", later_open, 0, 0, 0, ST_IDLE, "", {0} },
  { "SDRAM", later_open, 0, 0, 0, ST_IDLE, "", {0} },
  { "SPI flash", later_open, 0, 0, 0, ST_NA, "not fitted", {0, 0, 0, 0, 0, 0, 1, 0} },
  { "SD cards", later_open, 0, 0, 0, ST_IDLE, "", {0} },
  { "CF / IDE", later_open, 0, 0, 0, ST_IDLE, "", {0} },
  { "FT812", later_open, 0, 0, 0, ST_IDLE, "", {0} },
  { "Wi-Fi", later_open, 0, 0, 0, ST_IDLE, "", {0} },
  { "RTC / EEPROM", rtc_open, rtc_tick, 0, 0, ST_IDLE, "", {0} },
  { "Audio", aud_open, aud_tick, aud_key, aud_close, ST_IDLE, "", {0} },
  { "Tape", tape_open, tape_tick, 0, tape_close, ST_IDLE, "", {0} },
  { "MIDI", midi_open, midi_tick, midi_key, midi_close, ST_IDLE, "", {0} },
  { "Video", vid_open, 0, vid_key, vid_close, ST_IDLE, "", {0} },
  { "Input", in_open, in_tick, 0, 0, ST_IDLE, "", {0} },
  { "Buttons / LEDs", btn_open, btn_tick, btn_key, btn_close, ST_IDLE, "", {0} },
  { "LED matrix", mx_open, mx_tick, mx_key, mx_close, ST_IDLE, "", {0} },
  { "Floppy", later_open, 0, 0, 0, ST_IDLE, "", {0} },
};
const int ngroups = sizeof(groups) / sizeof(groups[0]);
