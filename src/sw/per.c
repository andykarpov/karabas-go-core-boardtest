// The checks of the storage and the modules (docs/PLAN.md Ф3) on bt_per (rtl/io/bt_per.v): the SD cards (SD1 of the
// MCU over RPC, the SD2 slot by SPI), the CF / IDE (read only: IDENTIFY and sectors), the FT812 (wake-up, REG_ID, its
// clock), the Wi-Fi modules (the ESP8266 by AT over its uart, the ESP32 of the Mini G by its SPI registers). Nothing
// is written to a card or a drive. A slot or a module with nothing in it is N/A, not FAIL (the user 2026-11-22).
#include "app.h"
#include "io.h"
#include "mmio.h"
#include "rpc.h"
#include "scr.h"
#include "util.h"

extern char board_name[8];
static char t[64];
static uint8_t blk[512];

static void row(int r, const char *label, const char *v, int color) { page_row(r, label, v, color); }

// "1.9 GB" / "512 MB" / "64 KB" of a size in KB
static char *fmt_kb(char *p, uint32_t kb) {
  if (kb >= 1024u * 1024u) { uint32_t m = kb / 1024u; p = fmt_u(p, m / 1024u); *p++ = '.'; p = fmt_u(p, m % 1024u * 10u / 1024u); return str_cpy(p, " GB"); }
  if (kb >= 1024u) { p = fmt_u(p, kb / 1024u); return str_cpy(p, " MB"); }
  p = fmt_u(p, kb); return str_cpy(p, " KB");
}

static int str_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int str_pre(const char *s, const char *p) { while (*p && *s == *p) { s++; p++; } return !*p; }
static void wait_ms(uint32_t ms) { uint32_t t0 = now_ms(); while (now_ms() - t0 < ms) ; }

// ---------------------------------------------------------------- the SPI master

static uint32_t spi_ctl;
static void spi_set(uint32_t ctl) { spi_ctl = ctl; IOW(PE_SPI_CTL, ctl); }
static void spi_cs(int on) { spi_set(on ? spi_ctl | PE_CS : spi_ctl & ~PE_CS); }
static void spi_wait(void) { while (IOR(PE_SPI_CTL) & 1) ; }
static uint8_t spi_x(uint8_t b) { IOW(PE_SPI_DATA, b); spi_wait(); return (uint8_t) IOR(PE_SPI_DATA); }
static void spi_read(uint8_t *d, int n) {   // n bytes of 0xFF out, the answers in
  IOR(PE_SPI_NEXT);
  for (int i = 0; i < n; i++) { spi_wait(); d[i] = (uint8_t) IOR(PE_SPI_NEXT); }
  spi_wait();
}

// ---------------------------------------------------------------- SD cards

#define SD_SLOW PE_HALF(93)   // 400 kHz while the card starts
#define SD_FAST PE_HALF(1)    // 18.75 MHz

static uint8_t sd_cmd(int n, uint32_t arg, uint8_t crc) {
  spi_x(0xFF);
  spi_x((uint8_t) (0x40 | n));
  for (int s = 24; s >= 0; s -= 8) spi_x((uint8_t) (arg >> s));
  spi_x(crc);
  if (n == 12) spi_x(0xFF);        // the stuff byte after CMD12
  uint8_t r = 0xFF;
  for (int i = 0; i < 10 && (r & 0x80); i++) r = spi_x(0xFF);
  return r;
}

static int sd_token(void) {        // the start of a data block, 0xFE; 0 on a timeout or an error token
  uint32_t t0 = now_ms();
  for (;;) {
    uint8_t b = spi_x(0xFF);
    if (b == 0xFE) return 1;
    if (b != 0xFF || now_ms() - t0 > 300) return 0;
  }
}

static int sd_block(uint8_t *d, int n) {   // a data block of n bytes and its CRC
  if (!sd_token()) return 0;
  spi_read(d, n);
  spi_x(0xFF); spi_x(0xFF);
  return 1;
}

// SD1: the card of the MCU over RPC SYS 5 SD_INFO (firmware PR): mounted, type, sectors, FS, total / free KB, label
static void sd1_row(group_t *g) {
  uint8_t b[48];
  uint16_t n = 0;
  int st = rpc_enabled() ? rpc_call(RPC_SYS, 5, 0, 0, b, sizeof(b) - 1, &n, 8000) : RPC_TIMEOUT;
  if (st == RPC_UNKNOWN) { g->v[0] = ST_NA_FW; row(0, "SD1", "N/A: the firmware has no SD_INFO (PR #167)", C_DIM); return; }
  if (st != RPC_OK || n < 15) { g->v[0] = ST_FAIL; row(0, "SD1", "no answer of the firmware", C_FAIL); return; }
  if (!b[0]) { g->v[0] = ST_FAIL; row(0, "SD1", "no card mounted (the cores come from it?)", C_FAIL); return; }
  static const char *fs[5] = { "?", "FAT12", "FAT16", "FAT32", "exFAT" };
  char *p = str_cpy(t, b[1] == 2 ? "SDHC " : b[1] == 1 ? "SDSC " : "");
  uint32_t sec = (uint32_t) b[2] | (uint32_t) b[3] << 8 | (uint32_t) b[4] << 16 | (uint32_t) b[5] << 24;
  p = fmt_kb(p, sec / 2); *p++ = ' ';
  p = str_cpy(p, fs[b[6] < 5 ? b[6] : 0]);
  uint32_t fr = (uint32_t) b[11] | (uint32_t) b[12] << 8 | (uint32_t) b[13] << 16 | (uint32_t) b[14] << 24;
  p = str_cpy(p, ", free "); p = fr == 0xFFFFFFFFu ? str_cpy(p, "?") : fmt_kb(p, fr);
  b[n] = 0;
  if (n > 15 && b[15]) { p = str_cpy(p, " \""); str_copy_n(p, (const char *) b + 15, 12); p += str_len(p); *p++ = '"'; *p = 0; }
  row(0, "SD1", t, C_TEXT);
  g->v[0] = ST_PASS;
}

// SD2: the slot of the FPGA (the firmware uses it only under the boot core)
static int sd2_check(void) {
  spi_set(PE_SD2 | PE_OWN_SD | SD_SLOW);
  if (!(IOR(PE_SPI_CTL) & 4)) { row(2, "SD2", "no card in the slot", C_DIM); return ST_NA; }
  row(2, "SD2", "card in the slot", C_TEXT);
  for (int i = 0; i < 10; i++) spi_x(0xFF);          // 80 clocks with CS high
  spi_cs(1);
  uint8_t r = 0;
  for (int i = 0; i < 20 && r != 1; i++) r = sd_cmd(0, 0, 0x95);
  if (r != 1) { spi_cs(0); row(3, "Start", "no answer to CMD0", C_FAIL); return ST_FAIL; }
  int v2 = 0, hc = 0;
  uint8_t o[4];
  if (!(sd_cmd(8, 0x1AA, 0x87) & 4)) { spi_read(o, 4); v2 = o[2] == 1 && o[3] == 0xAA; }
  uint32_t t0 = now_ms();
  do { sd_cmd(55, 0, 0x65); r = sd_cmd(41, v2 ? 0x40000000u : 0, 0x77); } while (r && now_ms() - t0 < 1500);
  if (r) { spi_cs(0); row(3, "Start", "ACMD41: the card stays idle", C_FAIL); return ST_FAIL; }
  if (v2 && !sd_cmd(58, 0, 0xFD)) { spi_read(o, 4); hc = (o[0] >> 6) & 1; }
  row(3, "Start", hc ? "SDHC / SDXC (v2)" : v2 ? "SDSC (v2)" : "SDSC (v1)", C_TEXT);
  spi_set(PE_SD2 | PE_OWN_SD | PE_CS | SD_FAST);
  // CID: the maker, the name
  uint8_t c[16];
  if (!sd_cmd(10, 0, 0x1B) && sd_block(c, 16)) {
    char *p = str_cpy(t, "maker 0x"); p = fmt_hex(p, c[0], 2); p = str_cpy(p, " \"");
    for (int i = 3; i < 8; i++) *p++ = c[i] >= 32 && c[i] < 127 ? (char) c[i] : '?';
    p = str_cpy(p, "\" rev "); p = fmt_u(p, c[8] >> 4); *p++ = '.'; fmt_u(p, c[8] & 15);
    row(4, "CID", t, C_TEXT);
  } else row(4, "CID", "not read", C_FAIL);
  // CSD: the size
  uint32_t kb = 0;
  if (!sd_cmd(9, 0, 0xAF) && sd_block(c, 16)) {
    if (c[0] >> 6) kb = ((((uint32_t) c[7] & 0x3F) << 16 | (uint32_t) c[8] << 8 | c[9]) + 1) * 512u;
    else {
      uint32_t cs = ((uint32_t) c[6] & 3) << 10 | (uint32_t) c[7] << 2 | c[8] >> 6;
      uint32_t mult = ((c[9] & 3) << 1 | c[10] >> 7) + 2, bl = c[5] & 15;
      kb = (cs + 1) << mult << bl >> 10;
    }
    fmt_kb(t, kb);
    row(5, "Size", t, C_TEXT);
  } else { spi_cs(0); row(5, "Size", "CSD not read", C_FAIL); return ST_FAIL; }
  // sector 0
  if (sd_cmd(17, 0, 0xFF) || !sd_block(blk, 512)) { spi_cs(0); row(6, "Sector 0", "not read", C_FAIL); return ST_FAIL; }
  row(6, "Sector 0", blk[510] == 0x55 && blk[511] == 0xAA ? "read, 55AA (a partition table / boot sector)" :
      "read (no 55AA: not partitioned?)", C_TEXT);
  // the speed: 128 KB by CMD18 at 18.75 MHz
  uint32_t u0 = R_US;
  int ok = !sd_cmd(18, 0, 0xFF);
  for (int i = 0; ok && i < 256; i++) ok = sd_block(blk, 512);
  uint32_t us = R_US - u0;
  sd_cmd(12, 0, 0x61);
  t0 = now_ms();
  while (spi_x(0xFF) != 0xFF && now_ms() - t0 < 300) ;
  spi_cs(0);
  spi_x(0xFF);
  if (!ok) { row(7, "Speed", "a read of 256 sectors failed", C_FAIL); return ST_FAIL; }
  char *p = fmt_u(t, us ? (uint32_t) (128000000ull / us) : 0);
  str_cpy(p, " KB/s (SPI 18.75 MHz, 128 KB)");
  row(7, "Speed", t, C_TEXT);
  return ST_PASS;
}

void sd_open(group_t *g) {
  sd1_row(g);
  int s2 = sd2_check();
  spi_set(0);
  int s1 = g->v[0];
  int st = s1 == ST_FAIL || s2 == ST_FAIL ? ST_FAIL : s1 == ST_PASS || s2 == ST_PASS ? ST_PASS : ST_NA;
  char *p = str_cpy(t, s1 == ST_PASS ? "SD1 ok" : s1 == ST_FAIL ? "SD1 FAIL" : "SD1 -");
  str_cpy(p, s2 == ST_PASS ? " SD2 ok" : s2 == ST_FAIL ? " SD2 FAIL" : " SD2 -");
  set_status(g, st, t);
  page_hint("Nothing is written to the cards");
}

// ---------------------------------------------------------------- CF / IDE

static uint16_t ide_rd(int reg) {
  IOW(PE_IDE_GO, (uint32_t) reg);
  while (IOR(PE_IDE_GO) & 1) ;
  return (uint16_t) IOR(PE_IDE_DATA);
}
static void ide_wr(int reg, uint8_t v) {
  IOW(PE_IDE_GO, (uint32_t) reg | 0x10u | (uint32_t) v << 16);
  while (IOR(PE_IDE_GO) & 1) ;
}
// wait until not BSY and (want DRQ: DRQ or ERR); the status, 0x100 on a timeout
static int ide_wait(int drq, uint32_t ms) {
  uint32_t t0 = now_ms();
  for (;;) {
    int s = ide_rd(7) & 0xFF;
    if (!(s & 0x80) && (!drq || (s & 9))) return s;
    if (now_ms() - t0 > ms) return 0x100;
  }
}

static char *ide_str(char *p, const uint16_t *w, int n) {   // an ATA string (two characters a word, high first), trimmed
  char *s = p;
  for (int i = 0; i < n; i++) { *p++ = (char) (w[i] >> 8); *p++ = (char) w[i]; }
  while (p > s && (p[-1] == ' ' || p[-1] == 0)) p--;
  for (char *q = s; q < p; q++) if (*q < 32 || *q > 126) *q = '?';
  *p = 0;
  return p;
}

static int cf_check(void) {
  // a device answers: two task file registers hold what is written (a floating bus does not)
  ide_wr(6, 0xE0);
  ide_wr(2, 0x55); ide_wr(3, 0xAA);
  int a = ide_rd(2) & 0xFF, b = ide_rd(3) & 0xFF;
  ide_wr(2, 0xAA); ide_wr(3, 0x55);
  int c = ide_rd(2) & 0xFF, d = ide_rd(3) & 0xFF;
  if (a != 0x55 || b != 0xAA || c != 0xAA || d != 0x55) {
    char *p = str_cpy(t, "no device (registers read ");
    p = fmt_hex(p, (uint32_t) a, 2); *p++ = ' '; p = fmt_hex(p, (uint32_t) b, 2); str_cpy(p, ")");
    row(0, "Device", t, C_DIM);
    return ST_NA;
  }
  row(0, "Device", "master answers (task file registers)", C_TEXT);
  int s = ide_wait(0, 3000);
  if (s & 0x100) { row(1, "Ready", "BSY stays set", C_FAIL); return ST_FAIL; }
  // IDENTIFY
  uint16_t *w = (uint16_t *) blk;
  ide_wr(7, 0xEC);
  s = ide_wait(1, 1000);
  if ((s & 0x100) || (s & 1)) { row(1, "IDENTIFY", (s & 0x100) ? "no data (timeout)" : "refused (ERR)", C_FAIL); return ST_FAIL; }
  uint16_t ones = 0, zeros = 0;
  for (int i = 0; i < 256; i++) { w[i] = ide_rd(0); ones |= w[i]; zeros |= (uint16_t) ~w[i]; }
  ide_str(t, w + 27, 20);
  row(1, "Model", t, C_TEXT);
  char *p = ide_str(t, w + 23, 4);
  p = str_cpy(p, ", serial "); ide_str(p, w + 10, 10);
  row(2, "Firmware", t, C_TEXT);
  uint32_t lba = (uint32_t) w[60] | (uint32_t) w[61] << 16;
  p = fmt_kb(t, lba / 2);
  p = str_cpy(p, ", "); p = fmt_u(p, lba); str_cpy(p, (w[49] & 0x200) ? " sectors, LBA" : " sectors, CHS only");
  row(3, "Size", t, C_TEXT);
  if (!(w[49] & 0x200) || !lba) { row(4, "Sector 0", "no LBA: not read", C_FAIL); return ST_FAIL; }
  // READ SECTORS: 0, then 128 for the speed
  uint32_t u0 = 0;
  for (int pass = 0; pass < 2; pass++) {
    int n = pass ? 128 : 1;
    ide_wr(2, (uint8_t) n); ide_wr(3, 0); ide_wr(4, 0); ide_wr(5, 0); ide_wr(6, 0xE0);
    if (pass) u0 = R_US;
    ide_wr(7, 0x20);
    for (int k = 0; k < n; k++) {
      s = ide_wait(1, 1000);
      if ((s & 0x100) || (s & 1)) { row(4 + pass, pass ? "Speed" : "Sector 0", "read failed", C_FAIL); return ST_FAIL; }
      for (int i = 0; i < 256; i++) { w[i] = ide_rd(0); ones |= w[i]; zeros |= (uint16_t) ~w[i]; }
    }
    if (!pass) row(4, "Sector 0", blk[510] == 0x55 && blk[511] == 0xAA ? "read, 55AA (a partition table / boot sector)" :
                   "read (no 55AA)", C_TEXT);
  }
  uint32_t us = R_US - u0;
  p = fmt_u(t, us ? (uint32_t) (64000000ull / us) : 0);
  str_cpy(p, " KB/s (PIO 0, 64 KB)");
  row(5, "Speed", t, C_TEXT);
  // the data lines: each one seen at 0 and at 1 in the words read
  uint16_t stuck = (uint16_t) ~(ones & zeros);
  if (stuck) {
    p = str_cpy(t, "never toggled:");
    for (int i = 15; i >= 0; i--) if ((stuck >> i) & 1) { p = str_cpy(p, " D"); p = fmt_u(p, (uint32_t) i); }
    row(6, "Data", t, C_FAIL);
    return ST_FAIL;
  }
  row(6, "Data", "D0..D15 all toggled", C_TEXT);
  return ST_PASS;
}

void cf_open(group_t *g) {
  int st = cf_check();
  set_status(g, st, st == ST_PASS ? "read ok" : st == ST_NA ? "no device" : "FAIL");
  page_hint("Read only: IDENTIFY and READ SECTORS (the card has your data)");
}

// ---------------------------------------------------------------- FT812

static void ft_host(uint8_t cmd, uint8_t par) {
  spi_cs(1); spi_x(cmd); spi_x(par); spi_x(0); spi_cs(0);
}
static uint32_t ft_rd32(uint32_t a) {
  spi_cs(1);
  spi_x((uint8_t) (a >> 16 & 0x3F)); spi_x((uint8_t) (a >> 8)); spi_x((uint8_t) a); spi_x(0);
  uint8_t b[4];
  spi_read(b, 4);
  spi_cs(0);
  return (uint32_t) b[0] | (uint32_t) b[1] << 8 | (uint32_t) b[2] << 16 | (uint32_t) b[3] << 24;
}

static int ft_check(void) {
  if (IOR(PE_SPI_CTL) & 2) { row(0, "FT812", "the MCU has the bus now (its FT mode): not checked", C_DIM); return ST_NA; }
  spi_set(PE_FT | PE_OWN_FT | PE_HALF(3));          // 9.4 MHz: below 11 MHz until the clock runs
  // as the firmware (lib/ft812): SLEEP, the external clock (FT_CLK_OUT 8 MHz) x 6 = 48 MHz, ACTIVE
  ft_host(0x42, 0); wait_ms(20);
  ft_host(0x44, 0); wait_ms(20);
  ft_host(0x61, 6 | 1 << 6); wait_ms(20);
  ft_host(0x00, 0); wait_ms(50);
  uint32_t t0 = now_ms(), id;
  while (((id = ft_rd32(0x302000)) & 0xFF) != 0x7C && now_ms() - t0 < 1000) wait_ms(1);
  char *p = str_cpy(t, "REG_ID 0x"); fmt_hex(p, id & 0xFF, 2);
  if ((id & 0xFF) != 0x7C) {
    str_cpy(t + str_len(t), (id & 0xFF) == 0xFF || !(id & 0xFF) ? ": no answer" : ": wrong");
    row(0, "SPI", t, C_FAIL);
    return ST_FAIL;
  }
  str_cpy(t + str_len(t), ": the chip answers");
  row(0, "SPI", t, C_TEXT);
  uint32_t cpu;                                     // its engines leave reset some ms after ACTIVE (the firmware waits 1 s)
  t0 = now_ms();
  while ((cpu = ft_rd32(0x302020) & 0xFF) != 0 && now_ms() - t0 < 1000) wait_ms(1);
  row(1, "CPU reset", cpu ? "held" : "0 (its engines run)", cpu ? C_FAIL : C_TEXT);
  // its clock: REG_CLOCK over 20 ms (FT_CLK_OUT 8 MHz x 6)
  uint32_t u0 = R_US, c0 = ft_rd32(0x302008);
  wait_ms(20);
  uint32_t u1 = R_US, c1 = ft_rd32(0x302008);
  uint32_t khz = u1 != u0 ? (uint32_t) ((uint64_t) (c1 - c0) * 1000u / (u1 - u0)) : 0;
  int ok = khz > 46600 && khz < 51600;
  p = fmt_u(t, khz / 1000); *p++ = '.'; p = fmt_u(p, khz % 1000 / 100);
  str_cpy(p, ok ? " MHz (its 8 MHz clock x 6)" : " MHz: not 48..49 (its 8 MHz clock?)");
  row(2, "Clock", t, ok ? C_TEXT : C_FAIL);
  row(3, "INT_N", (IOR(PE_SPI_CTL) & 8) ? "high (idle)" : "low", C_TEXT);
  return ok && !cpu ? ST_PASS : ST_FAIL;
}

void ft_open(group_t *g) {
  int st = ft_check();
  spi_set(0);
  set_status(g, st, st == ST_PASS ? "ID 7C, clock ok" : st == ST_NA ? "MCU has it" : "FAIL");
  page_hint("The FT812 is woken up as the firmware does; its picture: later");
}

// ---------------------------------------------------------------- Wi-Fi: the ESP8266 (AT) and the ESP32 (SPI)

static char line[64];
static int line_n;
static uint32_t at_t0, at_ms;
static const char *at_sent;
enum { AT_BUSY, AT_OK, AT_ERR, AT_TIMEOUT };

static void at_start(const char *cmd, uint32_t ms) {
  IOW(PE_UART_CTL, 2);
  for (const char *s = cmd; *s; s++) IOW(PE_UART, (uint8_t) *s);
  IOW(PE_UART, '\r'); IOW(PE_UART, '\n');
  at_sent = cmd; at_t0 = now_ms(); at_ms = ms; line_n = 0;
}


// the lines of the reply to cb until OK / ERROR / the timeout
static int at_poll(void (*cb)(const char *)) {
  for (int n = 0; n < 64; n++) {
    uint32_t v = IOR(PE_UART);
    if (!(v & 0x100)) break;
    char c = (char) v;
    if (c != '\r' && c != '\n') { if (line_n < (int) sizeof(line) - 1) line[line_n++] = c; continue; }
    line[line_n] = 0;
    if (!line_n) continue;
    line_n = 0;
    if (str_eq(line, "OK")) return AT_OK;
    if (str_eq(line, "ERROR") || str_eq(line, "FAIL")) return AT_ERR;
    if (!str_eq(line, at_sent) && cb) cb(line);   // the echo of the command is not a reply
  }
  return now_ms() - at_t0 > at_ms ? AT_TIMEOUT : AT_BUSY;
}

static int gmr_n;
static void gmr_line(const char *s) {
  if (gmr_n >= 2) return;
  row(1 + gmr_n, gmr_n ? "" : "Version", s, C_TEXT);
  gmr_n++;
}
static int ap_n, ap_best;
static char ap_name[24];
// +CWLAP:(3,"name",-61,"mac",1,...)
static void cwlap_line(const char *s) {
  if (!str_pre(s, "+CWLAP:(")) return;
  ap_n++;
  const char *q = s + 8;
  while (*q && *q != '"') q++;
  if (!*q) return;
  const char *name = ++q;
  while (*q && *q != '"') q++;
  int len = (int) (q - name);
  if (*q) q++;
  if (*q == ',') q++;
  int neg = *q == '-', rssi = 0;
  if (neg) q++;
  while (*q >= '0' && *q <= '9') rssi = rssi * 10 + (*q++ - '0');
  if (neg) rssi = -rssi;
  if (ap_n == 1 || rssi > ap_best) {
    ap_best = rssi;
    if (len > (int) sizeof(ap_name) - 1) len = sizeof(ap_name) - 1;
    for (int i = 0; i < len; i++) ap_name[i] = name[i];
    ap_name[len] = 0;
  }
}

// the ESP32: WRITE_REGS 01 reg 00 data..., READ_REGS 02 reg 00 then the bytes (ZiFi32 spi_slave of tslabs)
static void e32_wr(uint8_t reg, const uint8_t *d, int n) {
  spi_cs(1); spi_x(1); spi_x(reg); spi_x(0);
  for (int i = 0; i < n; i++) spi_x(d[i]);
  spi_cs(0);
}
static void e32_rd(uint8_t reg, uint8_t *d, int n) {
  spi_cs(1); spi_x(2); spi_x(reg); spi_x(0); spi_read(d, n); spi_cs(0);
}
static int e32_cmd(const uint8_t *d, int n) {     // command, status 0, its parameters; the status after it
  e32_wr(0, d, n);
  uint32_t t0 = now_ms();
  uint8_t s;
  do e32_rd(1, &s, 1); while ((s == 2 || s == 0) && now_ms() - t0 < 500);   // BUSY, or not taken yet
  return s;
}

static int esp32_check(void) {
  if (!str_eq(board_name, "Mini G")) { row(7, "ESP32", "N/A: only the Mini G has it", C_DIM); return ST_NA; }
  if (IOR(PE_SPI_CTL) & 2) { row(7, "ESP32", "the MCU has the FT812 bus now: not checked", C_DIM); return ST_NA; }
  spi_set(PE_ESP32 | PE_OWN_FT | PE_HALF(3));
  uint8_t v[8], c[3] = { 0x02, 0x00, 0x00 };      // GET_VER
  int s = e32_cmd(c, 2);
  if (s == 0 || s == 0xFF) { spi_set(0); row(7, "ESP32", "no answer on SPI (no module?)", C_DIM); return ST_NA; }
  if (s != 1) {
    char *p = str_cpy(t, "GET_VER: status 0x"); fmt_hex(p, (uint32_t) s, 2);
    spi_set(0); row(7, "ESP32", t, C_FAIL); return ST_FAIL;
  }
  e32_rd(2, v, 6);
  char *p = str_cpy(t, "ZiFi32 API "); p = fmt_u(p, v[0]);
  p = str_cpy(p, ", feature "); p = fmt_u(p, (uint32_t) (v[1] | v[2] << 8));
  p = str_cpy(p, ", version "); p = fmt_u(p, v[3]); *p++ = '.'; p = fmt_u(p, v[4]); *p++ = '.'; fmt_u(p, v[5]);
  row(7, "ESP32", t, C_TEXT);
  c[0] = 0x01; c[2] = 0x02;                        // GET_INFO_STR, the build
  if (e32_cmd(c, 3) == 1) {
    uint8_t n;
    e32_rd(2, &n, 1);
    if (n > 40) n = 40;
    e32_rd(3, blk, n);
    blk[n] = 0;
    p = str_cpy(t, "built ");
    str_copy_n(p, (const char *) blk, 41);
    row(8, "", t, C_TEXT);
  }
  spi_set(0);
  return ST_PASS;
}

// v[0] the step, v[1] its tries / time, v[2] the result of the ESP8266, v[3] of the ESP32, v[4] bytes while CTS held
void wifi_open(group_t *g) {
  g->v[0] = 0; g->v[1] = 0; g->v[2] = ST_RUN; g->v[3] = ST_RUN;
  gmr_n = ap_n = 0;
  row(0, "ESP8266", "AT ...", C_DIM);
  page_hint("The ESP8266 settings are not changed (no _DEF commands, no join)");
  at_start("AT", 500);
  set_status(g, ST_RUN, 0);
}

static void wifi_done(group_t *g) {
  int a = g->v[2], b = g->v[3];
  int st = a == ST_FAIL || b == ST_FAIL ? ST_FAIL : a == ST_PASS || b == ST_PASS ? ST_PASS : ST_NA;
  char *p = str_cpy(t, a == ST_PASS ? "8266 ok" : a == ST_FAIL ? "8266 FAIL" : "8266 -");
  str_cpy(p, b == ST_PASS ? " 32 ok" : b == ST_FAIL ? " 32 FAIL" : "");
  set_status(g, st, t);
  g->v[0] = 99;
}

void wifi_tick(group_t *g) {
  int r;
  switch (g->v[0]) {
    case 0:   // AT, three tries
      r = at_poll(0);
      if (r == AT_BUSY) return;
      if (r == AT_OK) { row(0, "ESP8266", "answers AT", C_TEXT); at_start("AT+GMR", 1500); g->v[0] = 1; return; }
      if (++g->v[1] < 3) { at_start("AT", 500); return; }
      row(0, "ESP8266", "no answer to AT (no module?)", C_DIM);
      g->v[2] = ST_NA; g->v[0] = 5;
      return;
    case 1:   // the version
      r = at_poll(gmr_line);
      if (r == AT_BUSY) return;
      if (r != AT_OK) { row(1, "Version", "AT+GMR failed", C_FAIL); g->v[2] = ST_FAIL; g->v[0] = 5; return; }
      at_start("AT+CWMODE_CUR=1", 1000); g->v[0] = 2;
      return;
    case 2:   // the station mode (not saved), then the scan
      if (at_poll(0) == AT_BUSY) return;
      row(4, "Scan", "...", C_DIM);
      at_start("AT+CWLAP", 10000); g->v[0] = 3;
      return;
    case 3:
      r = at_poll(cwlap_line);
      if (r == AT_BUSY) return;
      if (r != AT_OK || !ap_n) { row(4, "Scan", r == AT_TIMEOUT ? "no reply in 10 s" : "no networks found", C_FAIL); g->v[2] = ST_FAIL; }
      else {
        char *p = fmt_u(t, (uint32_t) ap_n); p = str_cpy(p, ap_n == 1 ? " network, best \"" : " networks, best \"");
        p = str_cpy(p, ap_name); p = str_cpy(p, "\" ");
        if (ap_best < 0) *p++ = '-';
        p = fmt_u(p, (uint32_t) (ap_best < 0 ? -ap_best : ap_best)); str_cpy(p, " dBm");
        row(4, "Scan", t, C_TEXT);
        g->v[2] = ST_PASS;
      }
      // CTS: held while the ESP answers AT+GMR, then let go
      IOW(PE_UART_CTL, 2);
      IOW(PE_UART_CTL, 1);
      { const char *s = "AT+GMR\r\n"; while (*s) IOW(PE_UART, (uint8_t) *s++); }
      g->v[1] = (int32_t) now_ms(); g->v[0] = 4;
      return;
    case 4:
      if (now_ms() - (uint32_t) g->v[1] < 300) return;
      g->v[4] = (int32_t) IOR(PE_UART_CTL);
      IOW(PE_UART_CTL, 0);
      wait_ms(300);
      {
        int after = (int) IOR(PE_UART_CTL);
        IOW(PE_UART_CTL, 2);
        row(5, "CTS", !after ? "no reply at all" : g->v[4] < 8 ? "honoured (the ESP waits while it is held)" :
            "not honoured: the ESP sends while it is held (its flow control off?)", g->v[4] < 8 && after ? C_TEXT : C_DIM);
      }
      g->v[0] = 5;
      return;
    case 5:
      g->v[3] = esp32_check();
      wifi_done(g);
      return;
    default: return;
  }
}

void wifi_close(group_t *g) { (void) g; spi_set(0); IOW(PE_UART_CTL, 2); }

// ---------------------------------------------------------------- Floppy (the Go): drive A, read only

static void fd_step(uint32_t ctl) {   // one step: DIR set, then a 3 us STEP pulse, 6 ms for the head
  IOW(PE_FDD, ctl);
  wait_ms(1);
  IOW(PE_FDD, ctl | FD_STEP);
  uint32_t u = R_US; while (R_US - u < 3) ;
  IOW(PE_FDD, ctl);
  wait_ms(6);
}
static int fd_tr00(void) { return (IOR(PE_FDD) >> 1) & 1; }

// v[0] the step: 0 the motor started, 1 counting the index, 2 done; v[1] its time, v[2] the index count at the start
void fdd_open(group_t *g) {
  g->v[0] = 0;
  IOW(PE_FDD, FD_A | FD_MOTOR);
  g->v[1] = (int32_t) now_ms();
  row(0, "Drive", "A: selected, motor on", C_TEXT);
  page_hint("Read only: nothing is written to the disk (put a disk in drive A)");
  set_status(g, ST_RUN, 0);
}

void fdd_tick(group_t *g) {
  uint32_t ms = now_ms() - (uint32_t) g->v[1];
  if (g->v[0] == 0) {
    if (ms < 1000) return;                 // the motor speeds up
    g->v[2] = (int32_t) (IOR(PE_FDD) >> 8);
    g->v[3] = (int32_t) IOR(PE_FDD_FLUX);
    g->v[1] = (int32_t) now_ms();
    g->v[0] = 1;
    return;
  }
  if (g->v[0] != 1 || ms < 2000) return;
  g->v[0] = 2;
  uint32_t idx = (IOR(PE_FDD) >> 8) - (uint32_t) g->v[2], flux = IOR(PE_FDD_FLUX) - (uint32_t) g->v[3];
  uint32_t rpm = idx * 30;                 // pulses in 2 s
  char *p = fmt_u(t, rpm); p = str_cpy(p, " rpm ("); p = fmt_u(p, idx); str_cpy(p, " index pulses in 2 s)");
  int rpm_ok = rpm >= 270 && rpm <= 330;
  row(1, "Speed", idx ? t : "no index pulses (no disk?)", idx ? (rpm_ok ? C_TEXT : C_FAIL) : C_DIM);
  p = fmt_u(t, flux / 2); str_cpy(p, " flux pulses / s");
  row(2, "Read data", t, flux ? C_TEXT : C_DIM);
  row(3, "Protect", (IOR(PE_FDD) >> 2) & 1 ? "write protected" : "writable", C_TEXT);
  // track 0: out until TR00 (at most 85 steps), then 2 in (TR00 goes off), back out
  int n = 0;
  while (!fd_tr00() && n < 85) { fd_step(FD_A | FD_MOTOR); n++; }
  int found = fd_tr00(), off = 0;
  if (found) {
    fd_step(FD_A | FD_MOTOR | FD_IN); fd_step(FD_A | FD_MOTOR | FD_IN);
    off = !fd_tr00();
    while (!fd_tr00() && n < 90) { fd_step(FD_A | FD_MOTOR); n++; }
  }
  p = str_cpy(t, found ? "found after " : "not found in 85 steps");
  if (found) { p = fmt_u(p, (uint32_t) n); str_cpy(p, off ? " steps, off after 2 in" : " steps, but stays on"); }
  row(4, "Track 0", t, found && off ? C_TEXT : C_FAIL);
  IOW(PE_FDD, 0);
  int none = !idx && !found;
  int st = none ? ST_NA : found && off && rpm_ok && flux ? ST_PASS : ST_FAIL;
  if (none) row(6, "", "no drive answers (or no disk)", C_DIM);
  set_status(g, st, st == ST_PASS ? "300 rpm, TR00" : none ? "no drive" : "FAIL");
}

void fdd_close(group_t *g) { (void) g; IOW(PE_FDD, 0); }
