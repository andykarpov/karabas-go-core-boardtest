// The memory checks (docs/PLAN.md Ф2). SRAM: the tester bt_sram does the accesses, the program the order and the
// diagnosis. A pass: the lines by direct accesses (data, byte lanes, address), then march C- on the backgrounds 0000
// and 5555, a random fill / verify (LFSR, another seed each pass), the address written in each halfword; then the
// margin: the last pattern read with one clock a read (13 ns: under the timing of the ucf, only shown). The passes go
// on until Space; the look is the approved one of docs/img/f0_mock_sram.png.
#include "app.h"
#include "io.h"
#include "scr.h"
#include "util.h"

static char t[80];

// ---------------------------------------------------------------- SRAM

static uint16_t sr_rd(uint32_t a) { IOW(SR_ADR, a); IOW(SR_GO, SR_DRD); while (IOR(SR_GO) & 1) {} return (uint16_t) IOR(SR_DAT); }
static void sr_wr(uint32_t a, uint32_t v, int lanes) { IOW(SR_ADR, a); IOW(SR_DAT, v & 0xFFFF); IOW(SR_GO, SR_DWR(lanes)); while (IOR(SR_GO) & 1) {} }

// the elements of a pass after the lines (step 1..): { background or -1, GO }
static const struct { int32_t bg; uint32_t go; } sr_steps[] = {
  { 0x0000, SR_EL(0, 0, 1, 0, 0) }, { -1, SR_EL(0, 1, 1, 0, 1) }, { -1, SR_EL(0, 1, 1, 1, 0) },
  { -1, SR_EL(1, 1, 1, 0, 1) }, { -1, SR_EL(1, 1, 1, 1, 0) }, { -1, SR_EL(1, 1, 0, 0, 0) },
  { 0x5555, SR_EL(0, 0, 1, 0, 0) }, { -1, SR_EL(0, 1, 1, 0, 1) }, { -1, SR_EL(0, 1, 1, 1, 0) },
  { -1, SR_EL(1, 1, 1, 0, 1) }, { -1, SR_EL(1, 1, 1, 1, 0) }, { -1, SR_EL(1, 1, 0, 0, 0) },
  { -1, SR_EL(0, 0, 1, 0, 2) }, { -1, SR_EL(0, 1, 0, 2, 0) },
  { -1, SR_EL(0, 0, 1, 0, 3) }, { -1, SR_EL(0, 1, 0, 3, 0) },
};
#define SR_NSTEPS ((int) (sizeof(sr_steps) / sizeof(sr_steps[0])))
#define SR_MARGIN (SR_NSTEPS + 1)          // the step of the margin check
#define SR_TOTAL  (SR_NSTEPS + 2)

static struct {
  int step;                 // 0 the lines, 1..SR_NSTEPS the elements, SR_MARGIN, -1 stopped
  int run;                  // the passes go on
  uint32_t passes, errors, mask, first_a, first_d;
  int first_v;
  uint32_t dbad, dstuck, dval, dopen;   // the data lines: bad, stuck (at dval), open
  uint32_t abad;            // the address lines found bad (bit n = MAn)
  int lanes_bad;            // bit 0 the low byte write, bit 1 the high one
  int lines_done;
  int margin;               // -1 not yet, 0 ok, else the errors of the one clock reads
  uint32_t drawn_ms;
} sr;

static void sr_lines(void) {
  // data: 1 << k at 0 and its complement at 1 (the bus driven the other way before the read of 0), then the reverse
  sr.dbad = sr.dstuck = sr.dval = sr.dopen = 0;
  for (int k = 0; k < 16; k++) {
    uint32_t b = 1u << k;
    sr_wr(0, b, 3); sr_wr(1, ~b, 3);
    uint32_t r1 = sr_rd(0);
    sr_wr(0, ~b, 3); sr_wr(1, b, 3);
    uint32_t r0 = sr_rd(0);
    sr.dbad |= ((r1 ^ b) | (r0 ^ ~b)) & 0xFFFF;
    int h = (r1 >> k) & 1, l = (r0 >> k) & 1;      // bit k read where 1 / 0 was written
    if (!h || l) {                                  // bit k itself fails
      if (h == l) { sr.dstuck |= b; if (h) sr.dval |= b; }
      else sr.dopen |= b;                           // reads the opposite: what the bus had before
    }
  }
  // byte lanes: 0x1234, then 0xAB written in the low byte only, 0xCD in the high one only
  sr.lanes_bad = 0;
  sr_wr(5, 0x1234, 3); sr_wr(5, 0x00AB, 1);
  if (sr_rd(5) != 0x12AB) sr.lanes_bad |= 1;
  sr_wr(5, 0xCD00, 2);
  if (sr_rd(5) != 0xCDAB) sr.lanes_bad |= 2;
  // address (only with good data lines): 0 at 0 and at each 1 << n, then FFFF at 1 << k: who else changed
  sr.abad = 0;
  if (!sr.dbad) {
    for (int k = -1; k < 21; k++) {
      sr_wr(0, 0, 3);
      for (int n = 0; n < 21; n++) sr_wr(1u << n, 0, 3);
      sr_wr(k < 0 ? 0 : 1u << k, 0xFFFF, 3);
      if (k >= 0 && sr_rd(0) != 0) sr.abad |= 1u << k;
      for (int n = 0; n < 21; n++) {
        uint32_t v = sr_rd(1u << n);
        if (n != k && v != 0) sr.abad |= (1u << n) | (k >= 0 ? 1u << k : 0);
        if (n == k && v != 0xFFFF) sr.abad |= 1u << n;
      }
    }
  }
  sr.lines_done = 1;
}

static int bit_low(uint32_t m) { int i = 0; while (m && !(m & 1)) { m >>= 1; i++; } return i; }
static int bits(uint32_t m) { int n = 0; while (m) { n += m & 1; m >>= 1; } return n; }

// the diagnosis line: the first fault that explains the rest
const char *sram_diag(void) {
  char *p = t;
  if (sr.dbad) {
    uint32_t lo = sr.dbad & 0xFF, hi = sr.dbad & 0xFF00;
    if (lo == 0xFF || hi == 0xFF00)
      return lo == 0xFF ? "the low byte chip does not answer: MRD_N0 / MWR_N0, its power or the chip" :
                          "the high byte chip does not answer: MRD_N1 / MWR_N1, its power or the chip";
    int k = bit_low(sr.dbad);
    p = str_cpy(p, "MD"); p = fmt_u(p, (uint32_t) k);
    if (bits(sr.dbad) == 2) {
      p = str_cpy(p, " and MD"); p = fmt_u(p, (uint32_t) bit_low(sr.dbad & ~(1u << k)));
      str_cpy(p, " fail together: a short between them? check the solder");
    } else if (bits(sr.dbad) > 2) str_cpy(p, " and more data lines fail: check the solder of the chips");
    else if (sr.dopen & (1u << k)) str_cpy(p, " open: it reads back the last bus value; check the pin and its solder");
    else { p = str_cpy(p, " stuck at "); p = fmt_u(p, (sr.dval >> k) & 1); str_cpy(p, ": check the pin and its solder"); }
    return t;
  }
  if (sr.lanes_bad)
    return sr.lanes_bad == 1 ? "a write of the low byte only fails: MWR_N0 (/WE of the low byte chip)" :
           sr.lanes_bad == 2 ? "a write of the high byte only fails: MWR_N1 (/WE of the high byte chip)" :
                               "a write of one byte fails in both chips: MWR_N0 / MWR_N1";
  if (sr.abad) {
    int k = bit_low(sr.abad);
    p = str_cpy(p, "MA"); p = fmt_u(p, (uint32_t) k);
    if (bits(sr.abad) == 2) {
      p = str_cpy(p, " and MA"); p = fmt_u(p, (uint32_t) bit_low(sr.abad & ~(1u << k)));
      str_cpy(p, " shorted: two addresses are one cell; check the solder");
    } else if (bits(sr.abad) == 1) str_cpy(p, " open or stuck: halves of the memory alias; check the pin and its solder");
    else str_cpy(p, " and more address lines fail: check the solder of the chips");
    return t;
  }
  if (sr.errors) {
    p = str_cpy(p, "cells fail (bits ");
    p = fmt_hex(p, sr.mask, 4);
    str_cpy(p, sr.mask & 0xFF ? (sr.mask & 0xFF00 ? "): both chips" : "): the low byte chip") : "): the high byte chip");
    return t;
  }
  if (sr.passes) return "no faults found";
  return "";
}

static void sr_dots(int r, int n, uint32_t bad, int done) {
  for (int i = 0; i < n; i++)
    scr_put(PX + 10 + i, PROW + r, T_DOT, !done ? C_DIM : (bad >> i) & 1 ? C_FAIL : C_PASS, C_NONE);
}

static const char *sr_mode(void) {
  int s = sr.step;
  if (s < 0) return sr.passes ? "stopped" : "-";
  if (s == 0) return "the lines: data, byte lanes, address";
  if (s <= 6) return "march C-, background 0000";
  if (s <= 12) return "march C-, background 5555";
  if (s <= 14) return "random (LFSR): fill, verify";
  if (s <= SR_NSTEPS) return "the address in each halfword";
  return "margin: reads of one clock (13 ns)";
}

static void sr_draw(void) {
  char *p;
  page_row(0, "Size", "4 MB (2 x 2M x 8, 10 ns)", C_TEXT);
  page_row(1, "Mode", sr_mode(), C_TEXT);
  fmt_u(t, sr.passes); page_row(2, "Passes", t, C_TEXT);
  fmt_u(t, sr.errors); page_row(3, "Errors", t, sr.errors ? C_FAIL : C_TEXT);
  if (sr.first_v) {
    uint32_t x = (sr.first_d ^ (sr.first_d >> 16)) & 0xFFFF;
    int b = bit_low(x);
    p = str_cpy(t, "0x"); p = fmt_hex(p, sr.first_a & 0x1FFFFF, 6); p = str_cpy(p, "  bit "); p = fmt_u(p, (uint32_t) b);
    p = str_cpy(p, " read "); fmt_u(p, (sr.first_d >> (16 + b)) & 1);
    page_row(4, "First", t, C_FAIL);
  } else page_row(4, "First", "-", C_DIM);
  page_row(6, "Data", "", C_TEXT);
  sr_dots(6, 16, sr.dbad | (sr.lines_done ? sr.mask : 0), sr.lines_done);
  page_row(7, "", "0123456789ABCDEF", C_DIM);
  page_row(9, "Address", "", C_TEXT);
  sr_dots(9, 21, sr.abad, sr.lines_done && !sr.dbad);
  page_row(10, "", "012345678901234567890", C_DIM);
  // progress of the pass
  int pm = 0;
  if (sr.step > 0) {
    uint32_t cur = IOR(SR_CUR) & 0x1FFFFF;
    int s = sr.step - 1;
    uint32_t dn = s < SR_NSTEPS && (sr_steps[s].go & 4);
    uint32_t f = (dn ? SR_WORDS - 1 - cur : cur) * 1000 / SR_WORDS;
    pm = (int) ((uint32_t) sr.step * 1000 + f) / SR_TOTAL;
  }
  scr_text_n(PX, PROW + 12, "Progress", 10, C_DIM, C_NONE);
  scr_meter(PX + 10, PROW + 12, 30, sr.step < 0 ? 0 : pm, C_ACTIVE);
  p = fmt_u(t, (uint32_t) (sr.step < 0 ? 0 : pm / 10)); str_cpy(p, "%   ");
  scr_text_n(PX + 41, PROW + 12, t, 5, C_TEXT, C_NONE);
  page_row(14, "Margin", sr.margin < 0 ? "-" : sr.margin ? "13 ns reads fail (shown only: the test reads in 27 ns)" :
           "13 ns reads pass too", sr.margin < 0 ? C_DIM : C_TEXT);
  page_row(16, "", sr.step >= 0 ? "Space: stop   Enter: from the start" : "Space: go on   Enter: from the start", C_DIM);
  page_hint(sram_diag());
}

static void sr_start(void) {
  sr.step = 0; sr.run = 1; sr.passes = 0; sr.errors = 0; sr.mask = 0; sr.first_v = 0; sr.margin = -1;
  sr.lines_done = 0; sr.dbad = sr.abad = 0; sr.lanes_bad = 0;
}

void sram_open(group_t *g) {
  if (g->status == ST_IDLE || sr.step < 0) sr_start();
  if (g->status == ST_IDLE) set_status(g, ST_RUN, 0);
  sr_draw();
}

// take the results of the tester into the totals of the pass
static void sr_take(void) {
  uint32_t e = IOR(SR_ERRORS);
  if (!e) return;
  sr.errors += e;
  sr.mask |= IOR(SR_MASK) & 0xFFFF;
  if (!sr.first_v) { sr.first_v = 1; sr.first_a = IOR(SR_FIRST_A); sr.first_d = IOR(SR_FIRST_D); }
}

void sram_tick(group_t *g) {
  if (sr.step < 0) return;
  if (IOR(SR_GO) & 1) {                       // an element runs: the progress now and then
    if (now_ms() - sr.drawn_ms > 200) { sr.drawn_ms = now_ms(); sr_draw(); }
    return;
  }
  if (sr.step == 0) {
    IOW(SR_RDCLK, 2);
    sr_lines();
    IOW(SR_LAST, SR_WORDS - 1);
    IOW(SR_GO, SR_CLEAR);
  } else if (sr.step <= SR_NSTEPS) {
    sr_take();
    IOW(SR_GO, SR_CLEAR);
  } else {
    // the margin check is over
    sr.margin = (int) IOR(SR_ERRORS);
    IOW(SR_RDCLK, 2);
    IOW(SR_GO, SR_CLEAR);
    sr.passes++;
    int bad = sr.dbad || sr.abad || sr.lanes_bad || sr.errors;
    if (bad) set_status(g, ST_FAIL, sr.dbad ? "data lines" : sr.lanes_bad ? "byte lanes" : sr.abad ? "address lines" : "cells");
    else if (g->status != ST_FAIL) {
      char *p = fmt_u(t, sr.passes); str_cpy(p, sr.passes == 1 ? " pass ok" : " passes ok");
      set_status(g, ST_PASS, t);
    }
    sr.step = sr.run ? 0 : -1;
    sr_draw();
    return;
  }
  // the next element
  sr.step++;
  if (sr.step <= SR_NSTEPS) {
    int s = sr.step - 1;
    if (s == 12) IOW(SR_SEED, 0x1234567u + sr.passes * 0x9E3779B9u);
    if (sr_steps[s].bg >= 0) IOW(SR_BG, (uint32_t) sr_steps[s].bg);
    IOW(SR_GO, sr_steps[s].go);
  } else {
    IOW(SR_RDCLK, 1);                        // the margin: the address pattern read with one clock a read
    IOW(SR_GO, SR_EL(0, 1, 0, 3, 0));
  }
  sr_draw();
}

void sram_key(group_t *g, int k) {
  if (k == K_ENTER) { sr_start(); sr_draw(); }
  else if (k == K_SPACE) {
    // stop at once (an element running ends by itself in a second), or go on with a new pass
    if (sr.step < 0) { sr.run = 1; sr.step = 0; if (g->status == ST_IDLE) set_status(g, ST_RUN, 0); }
    else { sr.run = 0; sr.step = -1; if (g->status == ST_RUN) set_status(g, ST_IDLE, "stopped"); }
    sr_draw();
  }
}

// ---------------------------------------------------------------- SDRAM
// The tester runs on its own clock (a DCM the program sets): a sweep goes over 84 .. 140 MHz (84: the clock of the cores
// and of the old ZXDOS test; the chips of the boards are slow ones). At each frequency the clock is set, the SDRAM
// initialised, the read delay found (a short LFSR check at each half clock, the middle of the window kept), then a
// pass: march C- on 0000, a random fill / verify, the address in each halfword (all 32 MB). The lines (data, DQM,
// address, bank) are checked by direct accesses at 84 MHz at the start of each sweep. FAIL: a bad line or errors at
// 120 MHz or below; errors at 130 / 140 are only shown (the margin of the chip).

static const struct { uint8_t m, d; const char *name; } sd_f[] = {
  { 28, 25, "84" }, { 4, 3, "100" }, { 22, 15, "110" }, { 8, 5, "120" }, { 26, 15, "130" }, { 28, 15, "140" } };
#define SD_NF 6
#define SD_NF_REQ 4             // 84 .. 120 MHz must pass (the user 2026-11-22)
static const uint8_t sd_seq[] = { 0, 1, 2, 3, 4, 5, 12, 13, 14, 15 };   // the elements of sr_steps of a pass
#define SD_NEL ((int) sizeof(sd_seq))

enum { SP_STOP, SP_CLK, SP_INIT, SP_CAL, SP_LINES, SP_EL };
static struct {
  int ph, fi, el, run;
  uint32_t sweeps, errors, mask, first_a, first_d;
  int first_v, first_f;
  uint32_t dbad, dstuck, dval, dopen, abad;   // abad: halfword address bits (column 0..8, row 9..21, bank 22, 23)
  int lanes_bad, lines_done;
  uint8_t res[SD_NF];       // the worst of all the sweeps: 0 not yet, 1 pass, 2 errors, 3 no read window
  uint8_t cres;             // of the frequency now: 0 so far good, 3 no read window
  uint8_t rdly[SD_NF];      // the read delay kept (half clocks)
  uint32_t fe[SD_NF];       // errors at each frequency
  uint32_t drawn_ms;
} sd;

static void sd_wait(void) { while (IOR(SD_GO) & (SD_ST_BUSY | SD_ST_PROG)) {} }
static void sd_go(uint32_t v) { sd_wait(); IOW(SD_GO, v); }
static uint16_t sd_rd(uint32_t a) { sd_wait(); IOW(SD_ADR, a); sd_go(SR_DRD); sd_wait(); return (uint16_t) IOR(SD_DAT); }
static void sd_wr(uint32_t a, uint32_t v, int lanes) { sd_wait(); IOW(SD_ADR, a); IOW(SD_DAT, v & 0xFFFF); sd_go(SR_DWR(lanes)); sd_wait(); }

static void sd_lines(void) {
  sd.dbad = sd.dstuck = sd.dval = sd.dopen = 0;
  for (int k = 0; k < 16; k++) {
    uint32_t b = 1u << k;
    sd_wr(0, b, 3); sd_wr(1, ~b, 3);
    uint32_t r1 = sd_rd(0);
    sd_wr(0, ~b, 3); sd_wr(1, b, 3);
    uint32_t r0 = sd_rd(0);
    sd.dbad |= ((r1 ^ b) | (r0 ^ ~b)) & 0xFFFF;
    int h = (r1 >> k) & 1, l = (r0 >> k) & 1;
    if (!h || l) {
      if (h == l) { sd.dstuck |= b; if (h) sd.dval |= b; }
      else sd.dopen |= b;
    }
  }
  sd.lanes_bad = 0;
  sd_wr(5, 0x1234, 3); sd_wr(5, 0x00AB, 1);
  if (sd_rd(5) != 0x12AB) sd.lanes_bad |= 1;
  sd_wr(5, 0xCD00, 2);
  if (sd_rd(5) != 0xCDAB) sd.lanes_bad |= 2;
  sd.abad = 0;
  if (!sd.dbad) {
    for (int k = -1; k < 24; k++) {
      sd_wr(0, 0, 3);
      for (int n = 0; n < 24; n++) sd_wr(1u << n, 0, 3);
      sd_wr(k < 0 ? 0 : 1u << k, 0xFFFF, 3);
      if (k >= 0 && sd_rd(0) != 0) sd.abad |= 1u << k;
      for (int n = 0; n < 24; n++) {
        uint32_t v = sd_rd(1u << n);
        if (n != k && v != 0) sd.abad |= (1u << n) | (k >= 0 ? 1u << k : 0);
        if (n == k && v != 0xFFFF) sd.abad |= 1u << n;
      }
    }
  }
  sd.lines_done = 1;
}

// the pins of the bad address bits: A (13) and BA (2)
static uint32_t sd_apins(void) { return (sd.abad & 0x1FF) | ((sd.abad >> 9) & 0x1FFF); }
static uint32_t sd_bapins(void) { return (sd.abad >> 22) & 3; }

// the read delay: an LFSR over two rows at each half clock; the middle of the longest passing run
static void sd_cal(void) {
  uint32_t ok = 0;
  IOW(SD_LAST, 1023);
  IOW(SD_SEED, 0x2468ACE1u);
  for (int h = 2; h < 16; h++) {
    sd_wait();
    IOW(SD_CFG, 3u | (uint32_t) h << 4);
    sd_go(SR_EL(0, 0, 1, 0, 2) | SR_CLEAR);
    sd_go(SR_EL(0, 1, 0, 2, 0));
    sd_wait();
    if (IOR(SD_ERRORS) == 0) ok |= 1u << h;
  }
  int best = 0, bl = 0;
  for (int h = 2; h < 16; h++) {
    int l = 0;
    while (h + l < 16 && (ok >> (h + l)) & 1) l++;
    if (l > bl) { bl = l; best = h; }
  }
  if (!bl) { sd.cres = 3; sd.rdly[sd.fi] = 0; return; }
  sd.rdly[sd.fi] = (uint8_t) (best + (bl - 1) / 2);
  sd_wait();
  IOW(SD_CFG, 3u | (uint32_t) sd.rdly[sd.fi] << 4);
  IOW(SD_LAST, SD_WORDS - 1);
}

const char *sdram_diag(void) {
  char *p = t;
  if (sd.dbad) {
    uint32_t lo = sd.dbad & 0xFF, hi = sd.dbad & 0xFF00;
    if (lo == 0xFF || hi == 0xFF00)
      return lo == 0xFF ? "the low byte (DQ0-7) does not answer: LDQM, its pins or the chip" :
                          "the high byte (DQ8-15) does not answer: UDQM, its pins or the chip";
    int k = bit_low(sd.dbad);
    p = str_cpy(p, "DQ"); p = fmt_u(p, (uint32_t) k);
    if (bits(sd.dbad) == 2) {
      p = str_cpy(p, " and DQ"); p = fmt_u(p, (uint32_t) bit_low(sd.dbad & ~(1u << k)));
      str_cpy(p, " fail together: a short between them? check the solder");
    } else if (bits(sd.dbad) > 2) str_cpy(p, " and more data lines fail: check the solder of the chip");
    else if (sd.dopen & (1u << k)) str_cpy(p, " open: it reads back the last bus value; check the pin and its solder");
    else { p = str_cpy(p, " stuck at "); p = fmt_u(p, (sd.dval >> k) & 1); str_cpy(p, ": check the pin and its solder"); }
    return t;
  }
  if (sd.lanes_bad)
    return sd.lanes_bad == 1 ? "a write of the low byte only fails: LDQM" :
           sd.lanes_bad == 2 ? "a write of the high byte only fails: UDQM" : "a write of one byte fails: LDQM / UDQM";
  uint32_t ap = sd_apins(), bp = sd_bapins();
  if (ap || bp) {
    int k;
    if (ap) { k = bit_low(ap); p = str_cpy(p, "A"); p = fmt_u(p, (uint32_t) k); ap &= ~(1u << k); }
    else { k = bit_low(bp); p = str_cpy(p, "BA"); p = fmt_u(p, (uint32_t) k); bp &= ~(1u << k); }
    if (ap || bp) {
      if (bits(ap) + bits(bp) == 1) {
        p = str_cpy(p, ap ? " and A" : " and BA"); p = fmt_u(p, (uint32_t) bit_low(ap ? ap : bp));
        str_cpy(p, " shorted? two addresses are one cell; check the solder");
      } else str_cpy(p, " and more address lines fail: check the solder of the chip");
    } else str_cpy(p, " open or stuck: parts of the memory alias; check the pin and its solder");
    return t;
  }
  int fbad = -1, fok = -1;
  for (int f = 0; f < SD_NF; f++) { if (sd.res[f] >= 2 && fbad < 0) fbad = f; if (sd.res[f] == 1) fok = f; }
  if (fbad >= 0 && fbad < SD_NF_REQ) {
    p = str_cpy(p, "errors at "); p = str_cpy(p, sd_f[fbad].name);
    str_cpy(p, sd.res[fbad] == 3 ? " MHz: no read works (CLK, the control lines, the chip)" : " MHz: cells fail (the chip?)");
    return t;
  }
  if (fbad >= 0) {
    p = str_cpy(p, "works up to "); p = str_cpy(p, fok >= 0 ? sd_f[fok].name : "-");
    p = str_cpy(p, sd.res[fbad] == 3 ? " MHz, no read delay works from " : " MHz, errors from ");
    p = str_cpy(p, sd_f[fbad].name); str_cpy(p, " MHz (the margin, shown only)");
    return t;
  }
  if (sd.sweeps) { p = str_cpy(p, "no faults found, works up to "); p = str_cpy(p, sd_f[fok >= 0 ? fok : 0].name); str_cpy(p, " MHz"); return t; }
  return "";
}

static const char *sd_mode(void) {
  switch (sd.ph) {
    case SP_STOP: return sd.sweeps ? "stopped" : "-";
    case SP_CLK: return "setting the clock";
    case SP_INIT: return "power up sequence";
    case SP_CAL: return "finding the read delay";
    case SP_LINES: return "the lines: data, DQM, address, bank";
    default: break;
  }
  int s = sd_seq[sd.el];
  return s < 6 ? "march C-, background 0000" : s < 14 ? "random (LFSR): fill, verify" : "the address in each halfword";
}

static void sd_draw(void) {
  char *p;
  page_row(0, "Size", "32 MB (4 banks x 8192 x 512 x 16)", C_TEXT);
  p = str_cpy(t, sd_f[sd.fi].name); p = str_cpy(p, " MHz, CL 3");
  if (sd.rdly[sd.fi]) { p = str_cpy(p, ", read "); p = fmt_u(p, sd.rdly[sd.fi] / 2u); str_cpy(p, sd.rdly[sd.fi] & 1 ? ".5 clocks after" : " clocks after"); }
  page_row(1, "Clock", sd.ph == SP_STOP && !sd.sweeps ? "-" : t, C_TEXT);
  page_row(2, "Mode", sd_mode(), C_TEXT);
  fmt_u(t, sd.sweeps); page_row(3, "Sweeps", t, C_TEXT);
  fmt_u(t, sd.errors); page_row(4, "Errors", t, sd.errors ? C_FAIL : C_TEXT);
  if (sd.first_v) {
    uint32_t x = (sd.first_d ^ (sd.first_d >> 16)) & 0xFFFF;
    int b = bit_low(x);
    p = str_cpy(t, "0x"); p = fmt_hex(p, sd.first_a & 0xFFFFFF, 6); p = str_cpy(p, " bit "); p = fmt_u(p, (uint32_t) b);
    p = str_cpy(p, " read "); p = fmt_u(p, (sd.first_d >> (16 + b)) & 1); p = str_cpy(p, " at "); p = str_cpy(p, sd_f[sd.first_f].name);
    str_cpy(p, " MHz");
    page_row(5, "First", t, C_FAIL);
  } else page_row(5, "First", "-", C_DIM);
  page_row(7, "DQ", "", C_TEXT);
  sr_dots(7, 16, sd.dbad, sd.lines_done);
  page_row(8, "", "0123456789ABCDEF", C_DIM);
  page_row(10, "A, BA", "", C_TEXT);
  sr_dots(10, 13, sd_apins(), sd.lines_done && !sd.dbad);
  for (int i = 0; i < 2; i++)
    scr_put(PX + 25 + i, PROW + 10, T_DOT, !(sd.lines_done && !sd.dbad) ? C_DIM : (sd_bapins() >> i) & 1 ? C_FAIL : C_PASS, C_NONE);
  page_row(11, "", "0123456789012  01", C_DIM);
  int pm = 0;
  if (sd.ph == SP_EL) {
    uint32_t cur = (IOR(SD_CUR) >> 16) & 0xFF;
    int dn = sr_steps[sd_seq[sd.el]].go & 4;
    uint32_t f = (dn ? 255 - cur : cur) * 1000 / 256;
    pm = (int) (((uint32_t) (sd.fi * SD_NEL + sd.el) * 1000 + f) / (SD_NF * SD_NEL));
  } else if (sd.ph != SP_STOP) pm = sd.fi * 1000 / SD_NF;
  scr_text_n(PX, PROW + 13, "Progress", 10, C_DIM, C_NONE);
  scr_meter(PX + 10, PROW + 13, 30, pm, C_ACTIVE);
  p = fmt_u(t, (uint32_t) (pm / 10)); str_cpy(p, "%   ");
  scr_text_n(PX + 41, PROW + 13, t, 5, C_TEXT, C_NONE);
  // the sweep: a frequency each 6 cells, its colour the result
  scr_text_n(PX, PROW + 15, "MHz", 10, C_DIM, C_NONE);
  for (int f = 0; f < SD_NF; f++)
    scr_text_n(PX + 10 + f * 6, PROW + 15, sd_f[f].name, 6,
               sd.res[f] == 1 ? C_PASS : sd.res[f] >= 2 ? C_FAIL : (sd.ph != SP_STOP && f == sd.fi) ? C_TEXT : C_DIM, C_NONE);
  page_row(17, "", sd.ph != SP_STOP ? "Space: stop   Enter: from the start" : "Space: go on   Enter: from the start", C_DIM);
  page_hint(sdram_diag());
}

static void sd_start(void) {
  memset(&sd, 0, sizeof(sd));
  sd.ph = SP_CLK; sd.run = 1;
}

void sdram_open(group_t *g) {
  if (g->status == ST_IDLE) sd_start();
  else if (sd.ph == SP_STOP) { sd.run = 1; sd.ph = SP_CLK; sd.fi = 0; }   // a new sweep, the counts kept
  if (g->status == ST_IDLE) set_status(g, ST_RUN, 0);
  sd_draw();
}

static void sd_take(void) {
  uint32_t e = IOR(SD_ERRORS);
  if (!e) return;
  sd.errors += e;
  sd.fe[sd.fi] += e;
  sd.mask |= IOR(SD_MASK) & 0xFFFF;
  if (!sd.first_v) { sd.first_v = 1; sd.first_a = IOR(SD_FIRST_A); sd.first_d = IOR(SD_FIRST_D); sd.first_f = sd.fi; }
}

// the frequency is over: its result, then the next one (or the end of the sweep)
static void sd_next_f(group_t *g) {
  uint8_t r = sd.cres ? sd.cres : sd.fe[sd.fi] ? 2 : 1;
  if (r > sd.res[sd.fi]) sd.res[sd.fi] = r;
  int bad_lines = sd.dbad || sd.abad || sd.lanes_bad;
  int bad_req = 0;
  for (int f = 0; f < SD_NF_REQ; f++) if (sd.res[f] >= 2) bad_req = 1;
  if (bad_lines || bad_req) set_status(g, ST_FAIL, sd.dbad ? "data lines" : sd.lanes_bad ? "DQM" : sd.abad ? "address lines" : "cells");
  if (++sd.fi < SD_NF) { sd.ph = SP_CLK; return; }
  sd.sweeps++;
  if (g->status != ST_FAIL) {
    int fok = 0;
    for (int f = 0; f < SD_NF; f++) if (sd.res[f] == 1) fok = f;
    char *p = str_cpy(t, "ok to "); p = str_cpy(p, sd_f[fok].name); str_cpy(p, " MHz");
    set_status(g, ST_PASS, t);
  }
  sd.fi = 0;
  sd.ph = sd.run ? SP_CLK : SP_STOP;
}

void sdram_tick(group_t *g) {
  if (sd.ph == SP_STOP) return;
  uint32_t s = IOR(SD_GO);
  if (s & (SD_ST_BUSY | SD_ST_PROG)) {
    if (now_ms() - sd.drawn_ms > 200) { sd.drawn_ms = now_ms(); sd_draw(); }
    return;
  }
  switch (sd.ph) {
  case SP_CLK:
    sd.cres = 0;
    sd.fe[sd.fi] = 0;
    IOW(SD_FREQ, (uint32_t) (sd_f[sd.fi].m - 1) | (uint32_t) (sd_f[sd.fi].d - 1) << 8);
    sd.ph = SP_INIT;
    break;
  case SP_INIT:
    if (!(s & SD_ST_LOCK)) { sd.cres = 3; sd_next_f(g); break; }
    IOW(SD_CFG, 3u | 7u << 4);
    sd_go(SD_INIT);
    sd.ph = SP_CAL;
    break;
  case SP_CAL:
    if (!(s & SD_ST_INIT)) { sd.cres = 3; sd_next_f(g); break; }
    if (sd.fi == 0) { sd.ph = SP_LINES; break; }
    sd_cal();
    if (sd.cres == 3) { sd_next_f(g); break; }
    sd.ph = SP_EL;
    sd.el = -1;
    break;
  case SP_LINES:
    // at 84 MHz with the delay of the datasheet (7 half clocks), before the delay is searched: a bad line is named
    // even when no delay works
    sd_lines();
    sd_cal();
    if (sd.cres == 3) { sd_next_f(g); break; }
    sd.ph = SP_EL;
    sd.el = -1;
    break;
  case SP_EL:
    if (sd.el >= 0) sd_take();
    if (++sd.el >= SD_NEL) { sd_next_f(g); break; }
    {
      int k = sd_seq[sd.el];
      if (k == 12) IOW(SD_SEED, 0x1234567u + sd.sweeps * 0x9E3779B9u + (uint32_t) sd.fi);
      if (sr_steps[k].bg >= 0) IOW(SD_BG, (uint32_t) sr_steps[k].bg);
      IOW(SD_LAST, SD_WORDS - 1);
      sd_go(sr_steps[k].go | SR_CLEAR);
    }
    break;
  }
  sd_draw();
}

void sdram_key(group_t *g, int k) {
  if (k == K_ENTER) { sd_start(); if (g->status != ST_RUN) set_status(g, ST_RUN, 0); sd_draw(); }
  else if (k == K_SPACE) {
    if (sd.ph == SP_STOP) { sd.run = 1; sd.ph = SP_CLK; sd.fi = 0; if (g->status == ST_IDLE) set_status(g, ST_RUN, 0); }
    else { sd.run = 0; sd.ph = SP_STOP; if (g->status == ST_RUN) set_status(g, ST_IDLE, "stopped"); }
    sd_draw();
  }
}
