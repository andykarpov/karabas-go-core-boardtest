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
