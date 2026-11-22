// The registers of bt_io (rtl/io/bt_io.v): MMIO 0x80000800..
#pragma once
#include "mmio.h"

#define IO_DNA_LO   MMIO(0x800) // R: the FPGA DNA bits 31..0
#define IO_DNA_HI   MMIO(0x804) // R: bit 31 valid, 24..0 the DNA bits 56..32
#define IO_BTN      MMIO(0x808) // R: bits 5..0 the buttons (0, 1 the board, 2..5 the LED matrix), bit 8 DVI only
#define IO_MATRIX   MMIO(0x80c) // W: a matrix frame of v1 { command, address, data }
#define IO_AUD_CTL  MMIO(0x810) // W: the sound (AUD_*)
#define IO_INC_L    MMIO(0x814) // W: the phase step of generator L (f = inc * 40909090 / 2^32)
#define IO_INC_R    MMIO(0x818) // W: of generator R
#define IO_ADC_PEAK MMIO(0x81c) // R: { right, left } the highest |ADC| since the last read (reading clears)
#define IO_ADC_ZC_L MMIO(0x820) // R: rising zero crossings of the left ADC channel (counting)
#define IO_ADC_ZC_R MMIO(0x824) // R: of the right one
#define IO_TAPE     MMIO(0x828) // R: bit 0 TAPE_IN, bits 31..8 its rising edges (counting)
#define IO_MIDI     MMIO(0x82c) // W: a byte to the SAM2695 (a 64 byte fifo)
#define IO_SAMPLES  MMIO(0x830) // R: 48 kHz ticks (counting)
#define IO_MIX_PEAK MMIO(0x834) // R: { right, left } the highest |mix| since the last read (reading clears)
#define IO_ADC_NOW  MMIO(0x838) // R: { right, left } the last ADC sample

// IO_AUD_CTL
#define AUD_GEN_L   0x0001
#define AUD_GEN_R   0x0002
#define AUD_SINE    0x0004
#define AUD_ADC     0x0008      // the ADC (line in, SAM2695) in the mix
#define AUD_MON(m)  ((m) << 4)  // tape monitor: 0 off, 1 in, 2 out, 3 both
#define AUD_TAPE_OUT 0x0040     // TAPE_OUT = the square of generator L
#define AUD_VOL(v)  ((v) << 8)
#define AUD_INC(hz) ((uint32_t) ((uint64_t) (hz) * 4294967296ull / 40909090u))

// matrix frames (kg_l2_aux): CTL bit 0 clear, 1 update, 2 brightness, 3 meter mode; BITPLAN byte a (0..7 the red
// rows of the left matrix, 8..15 its green, 16..31 the right one)
#define MX_CTL(v)        (0x600000u | (v))
#define MX_BITPLAN(a, v) (0x620000u | (uint32_t) (a) << 8 | (v))

// The SRAM tester bt_sram (rtl/mem/bt_sram.v): MMIO 0x80000900.. (offsets for IOW / IOR: the host build models it)
#ifdef HOST
void host_wr(uint32_t off, uint32_t v);
uint32_t host_rd(uint32_t off);
#define IOW(off, v) host_wr((off), (v))
#define IOR(off)    host_rd(off)
#else
#define IOW(off, v) (MMIO(off) = (v))
#define IOR(off)    MMIO(off)
#endif
#define SR_GO       0x900       // W: the operation (SR_EL / SR_DRD / SR_DWR); R: bit 0 busy
#define SR_ADR      0x904       // W: the halfword of a direct access
#define SR_CUR      0x904       // R: the halfword of the element now
#define SR_DAT      0x908       // W: the data of a direct write; R: the data of the last direct read
#define SR_BG       0x90c       // W: the background (patterns 0 / 1)
#define SR_ERRORS   0x90c       // R: errors
#define SR_SEED     0x910       // W: the LFSR start of an element
#define SR_MASK     0x910       // R: the wrong bits of all reads
#define SR_LAST     0x914       // W: the last halfword of an element
#define SR_FIRST_A  0x914       // R: the first error: bit 31 valid, its halfword
#define SR_RDCLK    0x918       // W: the clocks of a read (2; 1 = the margin check)
#define SR_FIRST_D  0x918       // R: the first error { read, expected }
#define SR_CLEAR    0x10000u
#define SR_DRD      2u
#define SR_DWR(lanes) (3u | (uint32_t) (lanes) << 9)
// an element: down, read (pattern rp), write (pattern wp); patterns 0 BG, 1 ~BG, 2 LFSR, 3 the address
#define SR_EL(dn, r, w, rp, wp) (1u | (dn) << 2 | (r) << 3 | (w) << 4 | (rp) << 5 | (wp) << 7)
#define SR_WORDS    0x200000u   // 2M halfwords

// The SDRAM tester bt_sdram (rtl/mem/bt_sdram.v): MMIO 0x80000a00.. (offsets for IOW / IOR)
#define SD_GO       0xa00       // W: the operation (SD_INIT / SR_EL / SR_DRD / SR_DWR + SR_CLEAR); R: SD_ST_*
#define SD_ADR      0xa04       // W: the halfword of a direct access
#define SD_CUR      0xa04       // R: the halfword of the element now (bits 23:16 steady enough)
#define SD_DAT      0xa08       // W / R: as SR_DAT
#define SD_BG       0xa0c
#define SD_ERRORS   0xa0c
#define SD_SEED     0xa10
#define SD_MASK     0xa10
#define SD_LAST     0xa14
#define SD_FIRST_A  0xa14
#define SD_CFG      0xa18       // W: bits 1:0 CL, 7:4 the read delay in half clocks
#define SD_FIRST_D  0xa18
#define SD_FREQ     0xa1c       // W: the DCM to 75 MHz x M / D: (M - 1) | (D - 1) << 8
#define SD_INIT     0x20000u
#define SD_ST_BUSY  1u
#define SD_ST_PROG  2u
#define SD_ST_LOCK  4u
#define SD_ST_INIT  8u
#define SD_WORDS    0x1000000u  // 16M halfwords

// The storage and the modules bt_per (rtl/io/bt_per.v): MMIO 0x80000b00.. (offsets for IOW / IOR)
#define PE_SPI_CTL  0xb00       // W: bits 1:0 the device (PE_SD2 / PE_FT / PE_ESP32), 2 CS on, 3 own SD2, 4 own the FT bus,
                                //    15:8 the half period - 1; R: bit 0 busy, 1 the MCU has the FT bus, 2 a card in SD2, 3 FT_INT_N
#define PE_SPI_DATA 0xb04       // W: a byte out; R: the byte of the last transfer
#define PE_SPI_NEXT 0xb08       // R: the byte of the last transfer, and 0xFF goes out
#define PE_UART     0xb10       // W: a byte to the ESP8266; R: bit 8 a byte (7..0, taken), 9 tx full, 10 bytes lost
#define PE_UART_CTL 0xb14       // W: bit 0 CTS held (stop), 1 the rx fifo emptied; R: bytes in the rx fifo
#define PE_IDE_GO   0xb20       // W: bits 2:0 the register, 3 CS1, 4 a write, 31:16 its data; R: bit 0 busy
#define PE_IDE_DATA 0xb24       // R: the data of the last read cycle
#define PE_IDE_RST  0xb28       // W: bit 0 RESET_N low
#define PE_SD2      1u
#define PE_FT       2u
#define PE_ESP32    3u
#define PE_CS       4u
#define PE_OWN_SD   8u
#define PE_OWN_FT   16u
#define PE_HALF(h)  ((uint32_t) (h) << 8)
#define PE_FDD      0xb30       // W: bits 1:0 drive selects, 2 motor, 3 DIR (1 = in), 4 STEP, 5 side 1 (pins high = active);
                                // R: bit 0 INDEX, 1 TR00, 2 WPRT, 3 RDATA, bits 31:8 the INDEX pulses
#define PE_FDD_FLUX 0xb34       // R: the RDATA pulses
#define FD_A        1u
#define FD_MOTOR    4u
#define FD_IN       8u
#define FD_STEP     16u
