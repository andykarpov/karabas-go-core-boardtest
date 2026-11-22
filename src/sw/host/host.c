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
    case 0x40: regs[off / 4] = ms * 1000u; break;                  // R_US: with R_MS
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

// ---------------- the SRAM tester (bt_sram) and the two chips, with one defect (as src/sim/mem/tb_sram.v) ----------------
// sr_def: 0 none, 1 MD K stuck at 1, 2 MD K open, 3 MD J, K shorted, 4 MA J, K shorted, 5 MA J open, 6 /WE high dead,
// 7 /OE low dead
static int sr_def, sr_j = 3, sr_k = 7;
static uint8_t *sr_lo, *sr_hi;
static uint16_t sr_f, sr_c;                     // the two sides of MD (an undriven side keeps its value)
static uint32_t sr_reg[8], sr_out[8];           // written / read registers
static uint32_t sr_ca(uint32_t a) {
  a &= 0x1FFFFF;
  if (sr_def == 4) { uint32_t b = (a >> sr_j) & (a >> sr_k) & 1; a = (a & ~(1u << sr_j) & ~(1u << sr_k)) | b << sr_j | b << sr_k; }
  if (sr_def == 5) a &= ~(1u << sr_j);
  return a;
}
static uint16_t sr_short(uint16_t v) {
  if (sr_def == 3) { int b = (v >> sr_j) & (v >> sr_k) & 1; v = (uint16_t) ((v & ~(1u << sr_j) & ~(1u << sr_k)) | b << sr_j | b << sr_k); }
  return v;
}
static void sr_join_from(int fpga) {           // join the sides, but the open line
  uint16_t m = sr_def == 2 ? (uint16_t) ~(1u << sr_k) : 0xFFFF;
  if (fpga) sr_c = (uint16_t) ((sr_c & ~m) | (sr_f & m)); else sr_f = (uint16_t) ((sr_f & ~m) | (sr_c & m));
  sr_f = sr_short(sr_f); sr_c = sr_short(sr_c);
}
static void sr_cwr(uint32_t a, uint16_t v, int lanes) {
  sr_f = v; sr_join_from(1);
  uint32_t ca = sr_ca(a);
  if (lanes & 1) sr_lo[ca] = (uint8_t) sr_c;
  if ((lanes & 2) && sr_def != 6) sr_hi[ca] = (uint8_t) (sr_c >> 8);
}
static uint16_t sr_crd(uint32_t a) {
  uint32_t ca = sr_ca(a);
  sr_c = (uint16_t) ((sr_c & (sr_def == 7 ? 0xFF : 0)) | (sr_def == 7 ? 0 : sr_lo[ca]) | sr_hi[ca] << 8);
  if (sr_def == 1) sr_c |= (uint16_t) (1u << sr_k);
  sr_join_from(0);
  return sr_f;
}
static uint16_t sr_pat(int p, uint32_t a, uint32_t l) {
  return p == 0 ? (uint16_t) sr_reg[3] : p == 1 ? (uint16_t) ~sr_reg[3] : p == 2 ? (uint16_t) l : (uint16_t) (a ^ (a >> 16));
}
static void sr_go(uint32_t v) {
  if (v & 0x10000) { sr_out[3] = sr_out[4] = sr_out[5] = sr_out[6] = 0; }
  int op = v & 3;
  if (op == 2) sr_out[2] = sr_crd(sr_reg[1]);
  else if (op == 3) sr_cwr(sr_reg[1], (uint16_t) sr_reg[2], (v >> 9) & 3);
  else if (op == 1) {
    int dn = (v >> 2) & 1, r = (v >> 3) & 1, w = (v >> 4) & 1, rp = (v >> 5) & 3, wp = (v >> 7) & 3;
    uint32_t last = sr_reg[5], l = sr_reg[4];
    for (uint32_t i = 0; i <= last; i++) {
      uint32_t a = dn ? last - i : i;
      if (r) {
        uint16_t got = sr_crd(a), e = sr_pat(rp, a, l);
        if (got != e) {
          sr_out[3]++; sr_out[4] |= (uint16_t) (got ^ e);
          if (!(sr_out[5] >> 31)) { sr_out[5] = 0x80000000u | a; sr_out[6] = (uint32_t) got << 16 | e; }
        }
      }
      if (w) sr_cwr(a, sr_pat(wp, a, l), 3);
      l = (l & 1) ? ((l >> 1) ^ 0x80200003u) : (l >> 1);
    }
    sr_out[1] = dn ? 0 : last;
  }
}
static void sr_reset(int def, int j, int k) {
  if (!sr_lo) { sr_lo = malloc(0x200000); sr_hi = malloc(0x200000); }
  for (uint32_t i = 0; i < 0x200000; i++) { sr_lo[i] = (uint8_t) (i * 37); sr_hi[i] = (uint8_t) (i * 11 + 5); }
  sr_def = def; sr_j = j; sr_k = k;
  memset(sr_reg, 0, sizeof(sr_reg)); memset(sr_out, 0, sizeof(sr_out));
  sr_reg[4] = 1; sr_reg[5] = 0x1FFFFF; sr_reg[6] = 2;
}
// ---------------- the SDRAM tester (bt_sdram) and the chip: elements over the first 1M halfwords only (time) ----------------
// sd_def: 0 none, 1 DQ K stuck at 1, 8 bit 3 flips in some cells above 135 MHz
static int sd_def, sd_k = 5;
static uint16_t *sd_mem;
static uint32_t sd_reg[8], sd_out[8], sd_mhz = 100;
static uint16_t sd_crd(uint32_t a) {
  uint16_t v = sd_mem[a & 0xFFFFFF];
  if (sd_def == 1) v |= (uint16_t) (1u << sd_k);
  if (sd_def == 8 && sd_mhz > 135 && (a & 0x3FF) == 0x155) v ^= 8;
  return v;
}
static void sd_cwr(uint32_t a, uint16_t v, int lanes) {
  uint16_t o = sd_mem[a & 0xFFFFFF];
  sd_mem[a & 0xFFFFFF] = (uint16_t) (((lanes & 1) ? (v & 0xFF) : (o & 0xFF)) | ((lanes & 2) ? (v & 0xFF00) : (o & 0xFF00)));
}
static void sd_go_m(uint32_t v) {
  if (v & 0x10000) { sd_out[3] = sd_out[4] = sd_out[5] = sd_out[6] = 0; }
  if (v & 0x20000) { sd_out[0] |= 8; return; }
  int op = v & 3;
  if (op == 2) sd_out[2] = sd_crd(sd_reg[1]);
  else if (op == 3) sd_cwr(sd_reg[1], (uint16_t) sd_reg[2], (v >> 9) & 3);
  else if (op == 1) {
    int dn = (v >> 2) & 1, r = (v >> 3) & 1, w = (v >> 4) & 1, rp = (v >> 5) & 3, wp = (v >> 7) & 3;
    uint32_t last = sd_reg[5] > 0xFFFFF ? 0xFFFFF : sd_reg[5], l = sd_reg[4];
    for (uint32_t i = 0; i <= last; i++) {
      uint32_t a = dn ? last - i : i;
      uint16_t bg = (uint16_t) sd_reg[3];
      if (r) {
        uint16_t got = sd_crd(a), e = rp == 0 ? bg : rp == 1 ? (uint16_t) ~bg : rp == 2 ? (uint16_t) l : (uint16_t) (a ^ (a >> 16));
        if (got != e) {
          sd_out[3]++; sd_out[4] |= (uint16_t) (got ^ e);
          if (!(sd_out[5] >> 31)) { sd_out[5] = 0x80000000u | a; sd_out[6] = (uint32_t) got << 16 | e; }
        }
      }
      if (w) sd_cwr(a, wp == 0 ? bg : wp == 1 ? (uint16_t) ~bg : wp == 2 ? (uint16_t) l : (uint16_t) (a ^ (a >> 16)), 3);
      l = (l & 1) ? ((l >> 1) ^ 0x80200003u) : (l >> 1);
    }
  }
}
static void sd_reset(int def, int k) {
  if (!sd_mem) sd_mem = malloc(0x1000000 * 2);
  for (uint32_t i = 0; i < 0x1000000; i++) sd_mem[i] = (uint16_t) (i * 2654435761u >> 16);
  sd_def = def; sd_k = k;
  memset(sd_reg, 0, sizeof(sd_reg)); memset(sd_out, 0, sizeof(sd_out));
  sd_out[0] = 4;   // locked
}
int per_wr(uint32_t off, uint32_t v);
int per_rd(uint32_t off, uint32_t *v);
extern int dev_sd2, dev_cf, dev_ft, dev_e32, dev_8266;
uint32_t host_ms(void) { return ms; }
void host_wr(uint32_t off, uint32_t v) {
  if (per_wr(off, v)) return;
  if (off >= 0xa00 && off < 0xa20) {
    int i = (int) (off - 0xa00) / 4;
    if (i == 0) sd_go_m(v);
    else if (i == 7) { uint32_t m = (v & 0xFF) + 1, d = ((v >> 8) & 0xFF) + 1; sd_mhz = 75 * m / d; sd_out[0] = 4; }
    else sd_reg[i] = v;
  }
  if (off >= 0x900 && off < 0x920) { int i = (int) (off - 0x900) / 4; if (i) sr_reg[i] = v; else sr_go(v); }
}
uint32_t host_rd(uint32_t off) {
  uint32_t pv;
  if (per_rd(off, &pv)) return pv;
  if (off >= 0xa00 && off < 0xa20) return off == 0xa04 ? sd_reg[5] : sd_out[(off - 0xa00) / 4];
  if (off >= 0x900 && off < 0x920) return sr_out[(off - 0x900) / 4];
  return 0;
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
  else if (svc == RPC_SYS && fn == 5) {   // SD_INFO: a 32 GB SDHC, FAT32, 21.4 GB free, "KARABAS"
    if (!fw_new) return RPC_UNKNOWN;
    uint32_t sec = 62333952u, tot = 31150080u, fr = 22437888u;
    o[0] = 1; o[1] = 2;
    for (int i = 0; i < 4; i++) { o[2 + i] = (uint8_t) (sec >> 8 * i); o[7 + i] = (uint8_t) (tot >> 8 * i); o[11 + i] = (uint8_t) (fr >> 8 * i); }
    o[6] = 3;
    strcpy((char *) o + 15, "KARABAS");
    n = 23;
  }
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
  if (!strcmp(sc, "rtc_oldfw") || !strcmp(sc, "leds_oldfw") || !strcmp(sc, "sd_none")) fw_new = 0;
  if (!strcmp(sc, "sd_none")) dev_sd2 = 0;
  if (!strcmp(sc, "cf_none")) dev_cf = 0;
  if (!strcmp(sc, "ft_fail")) dev_ft = 0;
  if (!strcmp(sc, "wifi_none")) { dev_8266 = 0; dev_e32 = 0; }
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
  else if (!strcmp(sc, "sram")) {
    // a good memory: the first pass over, the second one under way
    sr_reset(0, 0, 0);
    int i = find("SRAM");
    app_open(i);
    while (groups[i].status == ST_RUN) run(1);
    run(9);
  }
  else if (!strcmp(sc, "sram_fail")) {
    sr_reset(1, 0, 7);   // MD7 stuck at 1 (the mock of Ф0)
    int i = find("SRAM");
    app_open(i);
    while (groups[i].status == ST_RUN) run(1);
    run(9);
  }
  else if (!strcmp(sc, "sram_diag")) {
    // each defect of the model: the diagnosis names it
    static const struct { int def, j, k; const char *want; } d[] = {
      { 0, 0, 0, "no faults" }, { 1, 0, 7, "MD7 stuck at 1" }, { 2, 0, 12, "MD12 open" }, { 3, 2, 9, "MD2 and MD9" },
      { 4, 5, 17, "MA5 and MA17 shorted" }, { 5, 20, 0, "MA20 open" }, { 6, 0, 0, "high byte" }, { 7, 0, 0, "low byte chip" } };
    int i = find("SRAM"), bad = 0;
    for (unsigned n = 0; n < sizeof(d) / sizeof(d[0]); n++) {
      sr_reset(d[n].def, d[n].j, d[n].k);
      groups[i].status = ST_IDLE;
      app_open(i);
      while (groups[i].status == ST_RUN) run(1);
      press(K_ESC);
      const char *g = sram_diag();
      int ok = strstr(g, d[n].want) != 0;
      bad |= !ok;
      fprintf(stderr, "sram_diag: defect %d: %s [%s] %s\n", d[n].def, ok ? "ok" : "WRONG", groups[i].info, g);
    }
    fprintf(stderr, "sram_diag: %s\n", bad ? "FAIL" : "all ok");
  }
  else if (!strcmp(sc, "sdram") || !strcmp(sc, "sdram_slow") || !strcmp(sc, "sdram_fail")) {
    // a sweep over; the slow one fails above 135 MHz, the failed one has DQ5 stuck at 1
    sd_reset(!strcmp(sc, "sdram_slow") ? 8 : !strcmp(sc, "sdram_fail") ? 1 : 0, 5);
    int i = find("SDRAM");
    app_open(i);
    while (groups[i].status == ST_RUN) run(1);
    if (!strcmp(sc, "sdram_fail")) for (int n = 0; n < 40; n++) run(1);
    else press(K_SPACE);
    fprintf(stderr, "%s: [%s] %s\n", sc, groups[i].info, sdram_diag());
  }
  else if (!strncmp(sc, "sd", 2) && (sc[2] == 0 || sc[2] == '_')) { app_open(find("SD cards")); run(100); }
  else if (!strncmp(sc, "cf", 2)) { app_open(find("CF / IDE")); run(100); }
  else if (!strncmp(sc, "ft", 2)) { app_open(find("FT812")); run(100); }
  else if (!strncmp(sc, "wifi", 4)) {
    int i = find("Wi-Fi");
    app_open(i);
    for (int n = 0; n < 20000 && groups[i].status == ST_RUN; n++) run(1);
    run(100);
    fprintf(stderr, "%s: [%s]\n", sc, groups[i].info);
  }
  else if (!strcmp(sc, "auto") || !strcmp(sc, "auto_run")) {
    sr_reset(0, 3, 7); sd_reset(0, 5);
    press(K_A);
    int sd = find("SDRAM"), sr = find("SRAM");
    for (int n = 0; n < 400000; n++) {
      run(1);
      if (!strcmp(sc, "auto_run") && groups[sr].status == ST_RUN) { run(3); break; }
      if (strcmp(sc, "auto_run") && n > 1000 && groups[sd].status != ST_RUN && groups[sd].status != ST_IDLE) break;
    }
    if (strcmp(sc, "auto_run")) run(200);
    for (int i = 0; i < ngroups; i++) fprintf(stderr, "%s: %s %d [%s]\n", sc, groups[i].name, groups[i].status, groups[i].info);
  }
  else if (!strcmp(sc, "loop")) {   // two rounds and a half with a bad SDRAM line, then Esc
    sr_reset(0, 3, 7); sd_reset(1, 5);
    press(K_L);
    int sd = find("SDRAM"), rounds = 0, was = 0;
    for (int n = 0; n < 2000000 && rounds < 2; n++) {
      run(1);
      int now = groups[sd].status == ST_RUN;
      if (was && !now) rounds++;
      was = now;
    }
    run(500);
    press(K_ESC);
    fprintf(stderr, "loop: SDRAM %d [%s]\n", groups[sd].status, groups[sd].info);
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
