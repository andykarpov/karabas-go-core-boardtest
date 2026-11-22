// The fake devices of bt_per for the host build (host.c): an SD card on the SPI of the SD2 slot (SPI mode commands,
// CMD18 streams until CMD12), the FT812 (host commands, REG_ID, REG_CLOCK), the ESP32 (ZiFi32 registers, GET_VER,
// GET_INFO_STR), the ESP8266 (AT replies, CTS honoured), a CF card on the IDE bus (IDENTIFY, READ SECTORS).
// per_scenario() sets what is fitted.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../io.h"

extern uint32_t host_ms(void);
int dev_sd2 = 1, dev_cf = 1, dev_ft = 1, dev_e32 = 1, dev_8266 = 1;

static uint32_t ctl;
static uint8_t last = 0xFF;

// ---- the SD card
static uint8_t q[700];
static int qh, qt, sd_idle = 1, acmd = 0, streaming = 0, sd_tries = 0;
static uint8_t cmd[6];
static int cn = -1;
static uint32_t sd_lba;
static void put(uint8_t b) { if (qt < (int) sizeof(q)) q[qt++] = b; }
static void block(const uint8_t *d, int n) { put(0xFE); for (int i = 0; i < n; i++) put(d[i]); put(0x12); put(0x34); }
static void sector(uint32_t lba) {
  uint8_t d[512];
  for (int i = 0; i < 512; i++) d[i] = (uint8_t) ((lba * 131u + (uint32_t) i * 7u) ^ (uint32_t) (i >> 3));
  if (lba == 0) { d[510] = 0x55; d[511] = 0xAA; }
  block(d, 512);
}
static void sd_exec(void) {
  int n = cmd[0] & 0x3F;
  uint32_t arg = (uint32_t) cmd[1] << 24 | (uint32_t) cmd[2] << 16 | (uint32_t) cmd[3] << 8 | cmd[4];
  qh = qt = 0;
  put(0xFF);
  int a = acmd; acmd = 0;
  if (n == 12) { streaming = 0; put(0xFF); put(0x00); put(0x00); put(0x00); return; }
  if (n == 0) { sd_idle = 1; put(1); return; }
  if (n == 8) { put(1); put(0); put(0); put(1); put(0xAA); return; }
  if (n == 55) { acmd = 1; put(sd_idle); return; }
  if (n == 41 && a) { if (++sd_tries >= 3) sd_idle = 0; put(sd_idle); return; }
  if (n == 58) { put(0); put(0xC0); put(0xFF); put(0x80); put(0); return; }
  if (n == 10) { static const uint8_t cid[16] = { 0x03, 'S', 'D', 'S', 'L', '3', '2', 'G', 0x80, 1, 2, 3, 4, 0, 0x14, 1 };
                 put(0); block(cid, 16); return; }
  if (n == 9) { uint8_t csd[16] = { 0x40, 0x0E, 0, 0x32, 0x5B, 0x59, 0, 0, 0, 0, 0x7F, 0x80, 0x0A, 0x40, 0, 1 };
                uint32_t cs = 60799; csd[7] = (uint8_t) (cs >> 16); csd[8] = (uint8_t) (cs >> 8); csd[9] = (uint8_t) cs;
                put(0); block(csd, 16); return; }
  if (n == 17) { put(0); sector(arg); return; }
  if (n == 18) { put(0); streaming = 1; sd_lba = arg; return; }
  put(0x04);
}
static uint8_t sd_x(uint8_t b) {
  uint8_t out = 0xFF;
  if (qh < qt) out = q[qh++];
  else if (streaming) { qh = qt = 0; put(0xFF); sector(sd_lba++); out = q[qh++]; }
  if (cn < 0 && (b & 0xC0) == 0x40) cn = 0;
  if (cn >= 0) { cmd[cn++] = b; if (cn == 6) { cn = -1; sd_exec(); } }
  return out;
}

// ---- the FT812
static uint8_t fb[8];
static int fn;
static uint32_t ft_us0;
static uint8_t ft_x(uint8_t b) {
  uint8_t out = 0;
  if (fn >= 4 && fb[0] < 0x40) {
    uint32_t a = ((uint32_t) fb[0] << 16 | (uint32_t) fb[1] << 8 | fb[2]) + (uint32_t) (fn - 4);
    uint32_t v = a >> 2 == 0x302000 >> 2 ? 0x7C : a >> 2 == 0x302008 >> 2 ? (host_ms() - ft_us0) * 48000u : 0;
    out = dev_ft ? (uint8_t) (v >> (8 * (a & 3))) : 0x00;
  }
  if (fn < 8) fb[fn] = b;
  fn++;
  return out;
}

// ---- the ESP32
static uint8_t er[64];
static uint8_t eb[64];
static int en;
static uint8_t e32_x(uint8_t b) {
  uint8_t out = 0xFF;
  if (en >= 3 && eb[0] == 2) out = dev_e32 ? er[(eb[1] + en - 3) & 63] : 0xFF;
  if (en < 64) eb[en] = b;
  en++;
  return out;
}
static void e32_end(void) {
  if (!dev_e32 || en < 4 || eb[0] != 1) return;
  for (int i = 3; i < en; i++) er[(eb[1] + i - 3) & 63] = eb[i];
  if (eb[1] != 0) return;
  if (er[0] == 0x02) { uint8_t v[6] = { 1, 52, 0, 1, 0, 3 }; memcpy(er + 2, v, 6); er[1] = 1; }
  else if (er[0] == 0x01) { const char *s = "Nov 21 2026 19:12:03"; er[2] = (uint8_t) strlen(s); memcpy(er + 3, s, strlen(s)); er[1] = 1; }
  else er[1] = 0x80;
}

static void cs_edge(uint32_t old, uint32_t now) {
  if ((now & 4) && !(old & 4)) { fn = 0; en = 0; }
  if (!(now & 4) && (old & 4)) {
    if ((old & 3) == 2 && fn == 3 && dev_ft) { if (fb[0] == 0x00) ft_us0 = host_ms(); }
    if ((old & 3) == 3) e32_end();
  }
}

static uint8_t spi_xfer(uint8_t b) {
  if (!(ctl & 4)) return 0xFF;
  switch (ctl & 3) {
    case 1: return (ctl & 8) && dev_sd2 ? sd_x(b) : 0xFF;
    case 2: return (ctl & 16) ? ft_x(b) : 0xFF;
    case 3: return (ctl & 16) ? e32_x(b) : 0xFF;
  }
  return 0xFF;
}

// ---- the ESP8266
static char tx_line[80];
static int tx_n, cts_held;
static char rx[4096];
static int rxh, rxt;
static void say(const char *s) { while (*s && rxt < (int) sizeof(rx)) rx[rxt++] = *s++; }
static void at_line(const char *l) {
  if (!dev_8266) return;
  say(l); say("\r\n");
  if (!strcmp(l, "AT") || !strcmp(l, "AT+CWMODE_CUR=1")) say("\r\nOK\r\n");
  else if (!strcmp(l, "AT+GMR"))
    say("AT version:1.7.5.0(Oct  9 2021 09:26:04)\r\nSDK version:3.0.5-dev(52383f9)\r\ncompile time:Oct 15 2021 18:56:21\r\nOK\r\n");
  else if (!strcmp(l, "AT+CWLAP"))
    say("+CWLAP:(3,\"bench-2g\",-48,\"a0:b1:c2:d3:e4:f5\",6,-12,0)\r\n+CWLAP:(4,\"neighbour\",-77,\"10:20:30:40:50:60\",11,3,0)\r\n"
        "+CWLAP:(0,\"guest\",-83,\"10:20:30:40:50:61\",1,0,0)\r\n\r\nOK\r\n");
  else say("ERROR\r\n");
}

// ---- the CF card
static uint8_t tf[8] = { 0, 0, 0, 0, 0, 0, 0, 0x50 };
static uint16_t data[256 * 128];
static int dn, dlen, cf_left;
static void ata_str(uint16_t *w, const char *s, int words) {
  char b[64];
  memset(b, ' ', sizeof(b));
  memcpy(b, s, strlen(s));
  for (int i = 0; i < words; i++) w[i] = (uint16_t) ((uint8_t) b[2 * i] << 8 | (uint8_t) b[2 * i + 1]);
}
static void cf_cmd(uint8_t c) {
  dn = 0;
  if (c == 0xEC) {
    memset(data, 0, 512);
    data[0] = 0x848A;
    ata_str(data + 10, "BHZ081215213844", 10);
    ata_str(data + 23, "HDX 4.04", 4);
    ata_str(data + 27, "SanDisk SDCFH-004G", 20);
    data[49] = 0x0200;
    data[60] = (uint16_t) 7831152; data[61] = (uint16_t) (7831152 >> 16);
    dlen = 256; tf[7] = 0x58;
  } else if (c == 0x20) {
    int n = tf[2] ? tf[2] : 256;
    for (int s = 0; s < n && s < 128; s++)
      for (int i = 0; i < 256; i++) data[s * 256 + i] = (uint16_t) (((uint32_t) s * 40503u + (uint32_t) i * 2654435761u) >> 13);
    if (tf[3] == 0) data[255] = 0xAA55;
    dlen = 256 * (n < 128 ? n : 128); tf[7] = 0x58;
  } else tf[7] = 0x51;
  (void) cf_left;
}
static uint32_t ide_q;
static void ide_go(uint32_t v) {
  int reg = v & 7, cs1 = (v >> 3) & 1, wr = (v >> 4) & 1;
  if (!dev_cf) { ide_q = 0xFFFF; return; }
  if (cs1) { ide_q = tf[7]; return; }
  if (wr) { if (reg == 7) cf_cmd((uint8_t) (v >> 16)); else tf[reg] = (uint8_t) (v >> 16); return; }
  if (reg == 0) { ide_q = dn < dlen ? data[dn++] : 0; if (dn >= dlen) tf[7] = 0x50; return; }
  ide_q = tf[reg];
}

// ---- the registers
int per_wr(uint32_t off, uint32_t v) {
  if (off < 0xb00 || off >= 0xb40) return 0;
  switch (off) {
    case PE_SPI_CTL: cs_edge(ctl, v); ctl = v; break;
    case PE_SPI_DATA: last = spi_xfer((uint8_t) v); break;
    case PE_UART:
      if (v == '\n') { tx_line[tx_n] = 0; at_line(tx_line); tx_n = 0; }
      else if (v != '\r' && tx_n < 79) tx_line[tx_n++] = (char) v;
      break;
    case PE_UART_CTL: cts_held = v & 1; if (v & 2) rxh = rxt = 0; break;
    case PE_IDE_GO: ide_go(v); break;
  }
  return 1;
}
int per_rd(uint32_t off, uint32_t *v) {
  if (off < 0xb00 || off >= 0xb40) return 0;
  switch (off) {
    case PE_SPI_CTL: *v = (dev_sd2 ? 4u : 0u) | 8u; break;
    case PE_SPI_DATA: *v = last; break;
    case PE_SPI_NEXT: *v = last; last = spi_xfer(0xFF); break;
    case PE_UART: *v = !cts_held && rxh < rxt ? 0x100u | (uint8_t) rx[rxh++] : 0; break;
    case PE_UART_CTL: *v = cts_held ? 0 : (uint32_t) (rxt - rxh); break;
    case PE_IDE_GO: *v = 0; break;
    case PE_IDE_DATA: *v = ide_q; break;
    default: *v = 0;
  }
  return 1;
}
