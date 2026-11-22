`timescale 1ns / 1ps
`default_nettype none
/*
 bt_io.v

 The board I/O of the board test CPU (docs/PLAN.md Ф1), on clk (clk_cpu): MMIO 0x80000800..0x80000ffc of bt_soc,
 word registers (a = byte offset from 0x800):
   00 R  DNA_LO: the FPGA DNA bits 31..0          04 R  DNA_HI: bit 31 valid, bits 24..0 the DNA bits 56..32
   08 R  BTN: bits 5..0 kg_mcu2 BTNS (0, 1 the board, 2..5 the LED matrix add-on), bit 8 DVI only
   0c W  MATRIX: a frame of v1 { command, address, data } to the LED matrix (kg_l2_aux: 60 CTL, 61 PIXEL,
         62 BITPLAN); the audio peaks of the mix go the same way (audio_peaks), a frame of the CPU waits a clock
   10 W  AUD_CTL: the control of bt_audio (generators, sine, the ADC in the mix, tape monitor, TAPE_OUT, volume)
   14 W  AUD_INC_L: the phase step of generator L (f = inc * 40909090 / 2^32)     18 W  AUD_INC_R
   1c R  ADC_PEAK: { right, left } the highest absolute ADC sample since the last read (reading clears)
   20 R  ADC_ZC_L: the rising zero crossings of the left ADC channel (a hysteresis of +-ZC_HYST), counting
   24 R  ADC_ZC_R
   28 R  TAPE: bit 0 TAPE_IN now, bits 31..8 its rising edges (counting)
   2c W  MIDI: a byte to the SAM2695 (kg_midi_tx, a 64 byte fifo: the CPU paces the notes)
   30 R  SAMPLES: 48 kHz ticks of bt_audio seen (counting)
   34 R  MIX_PEAK: { right, left } the highest absolute sample of the mix since the last read (reading clears)
   38 R  ADC_NOW: { right, left } the last ADC sample
 The ADC and the mix come from bt_audio as samples held at each 48 kHz tick (smp_t).
*/
module bt_io #(
	parameter integer CLK_HZ = 75000000,
	parameter signed [15:0] ZC_HYST = 16'sd512
) (
	input wire clk,
	// the bus of bt_soc
	input wire we,
	input wire [8:0] a,                    // word address in the window
	input wire [31:0] wd,
	output reg [31:0] rd = 32'd0,          // a clock after the address
	input wire rd_go,                      // a read of this window (reading clears some)
	// kg_mcu2
	input wire [5:0] btns,
	input wire dvi_only,
	output wire [23:0] matrix_cmd,
	output wire matrix_cmd_wr,
	// bt_audio (clk_sys): its control, its samples
	output reg [15:0] aud_ctl = 16'h0008,  // the ADC in the mix at start (the line in heard, as the menu)
	output reg [31:0] aud_inc_l = 32'd0,
	output reg [31:0] aud_inc_r = 32'd0,
	input wire smp_t,
	input wire signed [15:0] smp_adc_l,
	input wire signed [15:0] smp_adc_r,
	input wire signed [15:0] smp_mix_l,
	input wire signed [15:0] smp_mix_r,
	// pins
	input wire tape_in,
	output wire midi_tx
);

// ---------------- DNA ----------------
// DNA_PORT: READ loads the 57 bits, then 57 SHIFTs, the first bit (56) on DOUT after the READ
reg [6:0] dna_n = 7'd0;
reg [1:0] dna_st = 2'd0;                   // 0 read, 1 shift, 2 done
reg dna_read = 1'b0, dna_shift = 1'b0;
reg [56:0] dna = 57'd0;
reg [4:0] dna_div = 5'd0;                  // DNA_PORT CLK up to 2 MHz: an edge every 32 clocks (1.17 MHz)
wire dna_dout;
reg dna_clk = 1'b0;
DNA_PORT #(.SIM_DNA_VALUE(57'h0123456789ABCDE)) dna_port (
	.DOUT(dna_dout), .CLK(dna_clk), .DIN(1'b0), .READ(dna_read), .SHIFT(dna_shift)
);
always @(posedge clk) begin
	dna_div <= dna_div + 5'd1;
	if (dna_div == 5'd0 && dna_st != 2'd2) begin
		if (!dna_clk) begin
			if (dna_st == 2'd0) begin
				dna_read <= 1'b1; dna_shift <= 1'b0; dna_clk <= 1'b1;
			end else begin
				// DOUT holds the next bit (56 after the READ edge, then one more after each SHIFT edge)
				dna <= { dna[55:0], dna_dout };
				dna_n <= dna_n + 7'd1;
				if (dna_n == 7'd56) dna_st <= 2'd2;
				else begin dna_read <= 1'b0; dna_shift <= 1'b1; dna_clk <= 1'b1; end
			end
		end else begin
			dna_clk <= 1'b0;
			if (dna_st == 2'd0) dna_st <= 2'd1;
		end
	end
end

// ---------------- the samples of bt_audio ----------------
reg [2:0] st = 3'b000;
always @(posedge clk) st <= { st[1:0], smp_t };
wire smp = st[2] != st[1];
reg signed [15:0] adc_l = 16'sd0, adc_r = 16'sd0, mix_l = 16'sd0, mix_r = 16'sd0;
reg [31:0] samples = 32'd0;
function [15:0] absv(input signed [15:0] v);
	absv = v[15] ? (v == 16'sh8000 ? 16'h7FFF : -v) : v;
endfunction
reg [15:0] apk_l = 16'd0, apk_r = 16'd0, mpk_l = 16'd0, mpk_r = 16'd0;
reg [31:0] zc_l = 32'd0, zc_r = 32'd0;
reg zl_neg = 1'b0, zr_neg = 1'b0;          // below -hyst seen since the last crossing

// ---------------- tape in ----------------
reg [2:0] tin = 3'b000;
always @(posedge clk) tin <= { tin[1:0], tape_in };
reg [23:0] tape_edges = 24'd0;

// ---------------- MIDI ----------------
reg [7:0] midi_d = 8'd0;
reg midi_wr = 1'b0;
kg_midi_tx #(.CLK_HZ(CLK_HZ)) midi (.clk(clk), .data(midi_d), .wr(midi_wr), .overflow(), .tx(midi_tx));

// ---------------- the matrix: the frames of the CPU and the peaks of the mix ----------------
wire pk_wr;
wire [23:0] pk_cmd;
audio_peaks #(.PERIOD_BITS(20)) peaks (.clk(clk), .audio_l(mix_l), .audio_r(mix_r), .cmd_wr(pk_wr), .cmd(pk_cmd));
reg cpu_pend = 1'b0;
reg [23:0] cpu_cmd = 24'd0;
assign matrix_cmd_wr = pk_wr | cpu_pend;
assign matrix_cmd = pk_wr ? pk_cmd : cpu_cmd;

// ---------------- the registers ----------------
reg [5:0] btns_s = 6'd0;
always @(posedge clk) btns_s <= btns;     // kg_mcu2 is on this clock: one flip-flop for the timing only

always @(posedge clk) begin
	midi_wr <= 1'b0;
	if (!pk_wr) cpu_pend <= 1'b0;
	if (smp) begin
		adc_l <= smp_adc_l; adc_r <= smp_adc_r;
		mix_l <= smp_mix_l; mix_r <= smp_mix_r;
		samples <= samples + 32'd1;
		if (absv(smp_adc_l) > apk_l) apk_l <= absv(smp_adc_l);
		if (absv(smp_adc_r) > apk_r) apk_r <= absv(smp_adc_r);
		if (absv(smp_mix_l) > mpk_l) mpk_l <= absv(smp_mix_l);
		if (absv(smp_mix_r) > mpk_r) mpk_r <= absv(smp_mix_r);
		if (smp_adc_l < -ZC_HYST) zl_neg <= 1'b1;
		else if (smp_adc_l > ZC_HYST && zl_neg) begin zl_neg <= 1'b0; zc_l <= zc_l + 32'd1; end
		if (smp_adc_r < -ZC_HYST) zr_neg <= 1'b1;
		else if (smp_adc_r > ZC_HYST && zr_neg) begin zr_neg <= 1'b0; zc_r <= zc_r + 32'd1; end
	end
	if (tin[1] && !tin[2]) tape_edges <= tape_edges + 24'd1;

	if (we) begin
		case (a)
			9'h003: begin cpu_cmd <= wd[23:0]; cpu_pend <= 1'b1; end
			9'h004: aud_ctl <= wd[15:0];
			9'h005: aud_inc_l <= wd;
			9'h006: aud_inc_r <= wd;
			9'h00B: begin midi_d <= wd[7:0]; midi_wr <= 1'b1; end
			default: ;
		endcase
	end
	case (a)
		9'h000: rd <= dna[31:0];
		9'h001: rd <= { dna_st == 2'd2, 6'd0, dna[56:32] };
		9'h002: rd <= { 23'd0, dvi_only, 2'd0, btns_s };
		9'h007: rd <= { apk_r, apk_l };
		9'h008: rd <= zc_l;
		9'h009: rd <= zc_r;
		9'h00A: rd <= { tape_edges, 7'd0, tin[2] };
		9'h00C: rd <= samples;
		9'h00D: rd <= { mpk_r, mpk_l };
		9'h00E: rd <= { adc_r, adc_l };
		default: rd <= 32'd0;
	endcase
	// reading clears the peaks (a sample of the same clock is lost: a meter does not see it)
	if (rd_go && !we && a == 9'h007) begin apk_l <= 16'd0; apk_r <= 16'd0; end
	if (rd_go && !we && a == 9'h00D) begin mpk_l <= 16'd0; mpk_r <= 16'd0; end
end

endmodule
