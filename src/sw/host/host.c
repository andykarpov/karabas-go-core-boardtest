// The host build of the board test program: main.c, tests.c, scr.c and util.c with a fake board (the registers of
// bt_soc / bt_io as an array, the RPC calls answered here, a clock that steps 1 ms per read of R_MS). A scenario
// opens a page, feeds keys / mouse / buttons, runs the loop, then writes the state of the screen (cells, palette,
// gradient, cursor) as tools/frame_model.py reads it: host/run.sh makes the frames of all the pages.
//   host SCENARIO OUT.state
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../app.h"
#include "../io.h"
#include "../mmio.h"
#include "../rpc.h"
#include "../scr.h"

void app_init(void);
void app_step(void);
void app_open(int i);
int app_page(void);

static uint32_t regs[0x14000 / 4];
static uint32_t ms, kbdn, tape_edges, zc_l, zc_r;
static uint32_t scratch, readc;

volatile uint32_t *host_reg(uint32_t off) {
  switch (off) {
    case 0x1c: ms++; regs[off / 4] = ms; break;                   // R_MS: 1 ms per read
    case 0x30: regs[off / 4] = kbdn; break;
    case 0x800: regs[off / 4] = 0x456789ABu; break;               // DNA
    case 0x804: regs[off / 4] = 0x80000000u | 0x0123u; break;
    case 0x820: zc_l = ms; regs[off / 4] = zc_l; break;           // a 1 kHz tone back on the left ADC
    case 0x824: zc_r = ms / 50; regs[off / 4] = zc_r; break;
    case 0x828: tape_edges = ms; regs[off / 4] = tape_edges << 8 | (ms & 1); break;
    case 0x81c: regs[off / 4] = (2000u << 16) | 21000u; break;    // ADC peaks
    case 0x80c: case 0x82c: return &scratch;                      // matrix, MIDI: written only
    case 0x34: case 0x3c: readc = regs[off / 4]; regs[off / 4] = 0; return &readc;   // MOUSE, WHEEL: clear on read
  }
  if (off >= sizeof(regs)) return &scratch;
  return &regs[off / 4];
}

// ---------------- RPC ----------------
int rpc_enabled(void) { return 1; }
static int fw_new = 1;   // the firmware has I2C_SCAN / LED
int rpc_call(uint8_t svc, uint8_t fn, const void *in, uint16_t in_len, void *out, uint16_t out_max,
             uint16_t *out_len, uint32_t timeout_ms) {
  (void) in_len; (void) timeout_ms; (void) out_max;
  uint8_t *o = (uint8_t *) out;
  uint16_t n = 0;
  if (svc == RPC_SYS && fn == 0) { memcpy(o, "26112201", 8); o[8] = 3; o[9] = 1; o[10] = 15; o[11] = 0; o[12] = 0; n = 13; }
  else if (svc == RPC_SYS && fn == 1) {
    uint32_t s = 59 + ms / 1000;
    uint8_t t[8] = { 0xEA, 0x07, 11, 22, 12, (uint8_t) (59 + s / 60) % 60, (uint8_t) (s % 60), 6 };
    memcpy(o, t, 8); n = 8;
  } else if (svc == RPC_SYS && fn == 3) {
    if (!fw_new) return RPC_UNKNOWN;
    memset(o, 0, 16);
    int a[] = { 0x50, 0x68, 0x70, 0x71 };
    for (int i = 0; i < 4; i++) o[a[i] >> 3] |= 1 << (a[i] & 7);
    n = 16;
  } else if (svc == RPC_SYS && fn == 4) { if (!fw_new) return RPC_UNKNOWN; }
  else if (svc == RPC_CFG && fn == 0) {
    // the Ocean theme of the bench
    static const char *v[7] = { "000608", "006a80", "0e2228", "00a0b8", "e0fcff", "ffffff", "5a8088" };
    for (int i = 0; i < 7; i++) { o[n++] = 1; strcpy((char *) o + n, v[i]); n += 7; }
  } else return RPC_UNKNOWN;
  if (out_len) *out_len = n;
  return RPC_OK;
}

// ---------------- the scenario ----------------
static void keys(uint32_t k0) { regs[0x28 / 4] = k0; regs[0x2c / 4] = 0; kbdn++; }
static void run(int n) { for (int i = 0; i < n; i++) app_step(); }
static void press(int code) { keys((uint32_t) code << 8); run(50); keys(0); run(50); }

static void dump(const char *path) {
  FILE *f = fopen(path, "w");
  fprintf(f, "grad %06x %06x\ncursor %08x\n", regs[0x18 / 4], regs[0x20 / 4], regs[0x44 / 4]);
  for (int i = 0; i < 16; i++) fprintf(f, "pal %d %07x\n", i, ((regs[(0x400 + 4 * i) / 4] >> 4) & 0xF000000) | (regs[(0x400 + 4 * i) / 4] & 0xFFFFFF));
  for (int i = 0; i < 80 * 30; i++) fprintf(f, "cell %d %05x\n", i, regs[(0x10000 + 4 * i) / 4] & 0x3FFFF);
  fclose(f);
}

static int find(const char *name) {
  for (int i = 0; i < ngroups; i++) if (!strcmp(groups[i].name, name)) return i;
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 3) return 1;
  const char *sc = argv[1];
  if (!strcmp(sc, "rtc_oldfw") || !strcmp(sc, "leds_oldfw")) fw_new = 0;
  app_init();
  if (!strcmp(sc, "main")) {
    // a board half way through: some results
    static const struct { const char *g; int st; const char *info; } r[] = {
      { "FPGA / MCU", ST_PASS, "DNA 456789AB" }, { "RTC / EEPROM", ST_PASS, "RTC, 24C08" }, { "Audio", ST_PASS, "L, R, loopback" },
      { "Tape", ST_RUN, 0 }, { "MIDI", ST_PASS, "SAM2695 on ADC" }, { "Video", ST_FAIL, "picture wrong" },
      { "Input", ST_PASS, "kbd, mouse" }, { "Buttons / LEDs", ST_ASK, 0 }, { "LED matrix", ST_NA, "not fitted" } };
    for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
      group_t *g = &groups[find(r[i].g)];
      g->status = (uint8_t) r[i].st;
      if (r[i].info) strcpy(g->info, r[i].info);
    }
    press(K_DOWN); press(K_DOWN); press(K_UP);   // redraws the main screen with the results
    app_open(0); press(K_ESC);
  } else if (!strcmp(sc, "fpga")) { app_open(find("FPGA / MCU")); run(100); }
  else if (!strcmp(sc, "rtc") || !strcmp(sc, "rtc_oldfw")) { app_open(find("RTC / EEPROM")); run(3000); }
  else if (!strcmp(sc, "leds") || !strcmp(sc, "leds_oldfw")) {
    app_open(find("Buttons / LEDs"));
    regs[0x808 / 4] = 1; run(100); regs[0x808 / 4] = 0; run(100); regs[0x808 / 4] = 2; run(100);   // BTN 1 pressed, released, BTN 2 held
  }
  else if (!strcmp(sc, "matrix")) { app_open(find("LED matrix")); regs[0x808 / 4] = 1 << 3; run(1500); }
  else if (!strcmp(sc, "audio")) { app_open(find("Audio")); run(8000); }
  else if (!strcmp(sc, "tape")) { app_open(find("Tape")); run(3000); }
  else if (!strcmp(sc, "midi")) {
    regs[0x81c / 4] = 0;
    app_open(find("MIDI")); run(3000);
  }
  else if (!strcmp(sc, "video")) { app_open(find("Video")); run(100); }
  else if (!strcmp(sc, "video_bars")) { app_open(find("Video")); press(K_ENTER); run(100); }
  else if (!strcmp(sc, "video_ramp")) { app_open(find("Video")); press(K_ENTER); press(K_SPACE); press(K_SPACE); run(100); }
  else if (!strcmp(sc, "input")) {
    app_open(find("Input"));
    keys(0x2C060500u); keys(0x2C060502u); run(200);                                           // B, C, Space with LShift
    regs[0x34 / 4] = (1u << 24) | (12u << 12) | 30u; run(50);
    regs[0x38 / 4] = 0x00410003u; run(200);                                                // joy L on + up + A, R on
  }
  else if (!strcmp(sc, "input_esc")) {
    // a short Esc stays on the page, a held one (over a second) leaves it
    int i = find("Input");
    app_open(i);
    press(K_ESC); run(2000);
    fprintf(stderr, "input_esc: after a short Esc the page is %s\n", app_page() == i ? "open" : "left");
    keys((uint32_t) K_ESC << 8); run(1500); keys(0); run(50);
    fprintf(stderr, "input_esc: after a held Esc the page is %s\n", app_page() == i ? "open" : "left");
  }
  regs[0x44 / 4] = 0;   // no cursor in the frames
  dump(argv[2]);
  return 0;
}
