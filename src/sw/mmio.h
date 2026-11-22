// The registers of bt_soc (rtl/soc/bt_soc.v): word registers at 0x80000000.
#pragma once
#include <stdint.h>

#ifdef HOST
// the host build (host/): the registers and the cells are arrays there
volatile uint32_t *host_reg(uint32_t off);
#define MMIO(off) (*host_reg(off))
#else
#define MMIO(off) (*(volatile uint32_t *) (0x80000000u + (off)))
#endif
#define R_DBG    MMIO(0x00) // W: a byte of the debug console
#define R_FRAMES MMIO(0x08) // R: frames begun
#define R_CYCLES MMIO(0x0c) // R: clocks since the start
#define R_SHOW   MMIO(0x10) // W: bit 0 the tile screen on (0: black)
#define R_GRAD_TOP MMIO(0x18) // W: the top colour of the background gradient
#define R_GRAD_BOT MMIO(0x20) // W: its bottom colour (3/4 of the height and below)
#define R_MS     MMIO(0x1c) // R: milliseconds since the start
#define R_KBD0   MMIO(0x28) // R: { key 3, key 2, key 1, modifiers }
#define R_KBD1   MMIO(0x2c) // R: { 0, key 6, key 5, key 4 }
#define R_KBDN   MMIO(0x30) // R: keyboard reports seen
#define R_MOUSE  MMIO(0x34) // R: buttons 31..24, dy 23..12, dx 11..0 (reading clears)
#define R_JOY    MMIO(0x38) // R: { joy_r, joy_l }
#define R_WHEEL  MMIO(0x3c) // R: the wheel sum 11..0, + away from the user (reading clears)
#define R_US     MMIO(0x40) // R: microseconds since the start
#define R_CURSOR MMIO(0x44) // W: bit 31 shown, 25..16 y, 10..0 x (the tip of the arrow)
#define R_PAL(i) MMIO(0x400 + 4 * (i)) // W: colour i of the cells: 0xARRGGBB, alpha 0..15 (0 transparent)
#define R_CELL(i) MMIO(0x10000 + 4 * (i)) // R/W: cell i = row * 80 + column: { bg 4, fg 4, tile 10 }

// the mailbox of kg_l2_rpc: byte k at 0x80002000 + 4 k (low byte of a word)
#define RPC(k)   MMIO(0x2000 + 4 * (k))
#define RPC_REQ   0x000 // the payload of the request
#define RPC_RESP  0x200 // the payload of the response
#define RPC_EVT   0x400 // the payload of an event
#define RPC_ID    0x600
#define RPC_SVC   0x601
#define RPC_FN    0x602
#define RPC_LEN   0x603 // 2 bytes
#define RPC_CALL  0x605
#define RPC_FLAGS 0x608 // bit 0 busy, 1 response in, 2 event in, 3 enabled
#define RPC_RST   0x609
#define RPC_RLEN  0x60A // 2 bytes
#define RPC_EDONE 0x614
#define RPC_FWVER 0x618

#define SCR_W 800
#define SCR_H 600
#define COLS 80
#define ROWS 30
#define CELL_W 10
#define CELL_H 20
