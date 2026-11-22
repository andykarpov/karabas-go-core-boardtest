`timescale 1ns / 1ps
`default_nettype none
/*
 bt_audio.v

 The sound of the board test (docs/PLAN.md Ф1), on clk (clk_sys, 40.91 MHz):
 - two tone generators L / R: a 32-bit phase accumulator each (f = inc * clk / 2^32), a sine (a quarter wave of
   64 steps, bt_qsin.vh, mirrored) or a square, times the volume / 256;
 - the mix to the DACs and HDMI: the generators + the ADC (the line in and the SAM2695; ctl[3], off while a
   tone goes out to a cable that loops back into the ADC) + the tape monitor (kg_tape_monitor, ctl[5:4]), with
   saturation;
 - TAPE_OUT: the square of generator L while ctl[6], else 0;
 - every 48 kHz tick (852 clocks) the ADC and the mix are held and smp_t toggles, for bt_io on clk_cpu (the held
   values are stable for 852 clocks; the crossing is bounded by TS_SYS_TO_CPU).
 ctl / inc_l / inc_r come from clk_cpu registers that change only between the tests (quasi-static).

 ctl: bit 0 generator L on, 1 generator R on, 2 sine (0 square), 3 the ADC in the mix, 5..4 tape monitor
 (0 off, 1 in, 2 out, 3 both), 6 TAPE_OUT = the square of generator L, 15..8 volume.

 The table (bt_qsin.vh): round(32767 * sin((i + 0.5) / 64 * pi / 2)), i = 0..63.
*/
module bt_audio #(
	parameter integer CLK_HZ = 40909090
) (
	input wire clk,
	input wire [15:0] ctl,
	input wire [31:0] inc_l,
	input wire [31:0] inc_r,
	input wire signed [15:0] adc_l,
	input wire signed [15:0] adc_r,
	input wire tape_in,                     // the pin (synchronised here)
	output reg signed [15:0] out_l = 16'sd0,
	output reg signed [15:0] out_r = 16'sd0,
	output reg tape_out = 1'b0,
	output reg smp_t = 1'b0,
	output reg signed [15:0] smp_adc_l = 16'sd0,
	output reg signed [15:0] smp_adc_r = 16'sd0,
	output reg signed [15:0] smp_mix_l = 16'sd0,
	output reg signed [15:0] smp_mix_r = 16'sd0
);

`include "bt_qsin.vh"

// the control into this clock (quasi-static: two flip-flops for the metastability only)
reg [15:0] ctl_s1 = 16'd0, c = 16'd0;
reg [31:0] il_s1 = 32'd0, il = 32'd0, ir_s1 = 32'd0, ir = 32'd0;
always @(posedge clk) begin
	ctl_s1 <= ctl; c <= ctl_s1;
	il_s1 <= inc_l; il <= il_s1;
	ir_s1 <= inc_r; ir <= ir_s1;
end

// ---------------- the generators ----------------
reg [31:0] ph_l = 32'd0, ph_r = 32'd0;
always @(posedge clk) begin
	ph_l <= c[0] ? ph_l + il : 32'd0;
	ph_r <= c[1] ? ph_r + ir : 32'd0;
end

// the sample of a phase: the quarter by bits 31..30, the step by bits 29..24
function signed [15:0] wave(input [31:0] ph, input sine);
	reg [5:0] i;
	reg [14:0] m;
	begin
		i = ph[30] ? ~ph[29:24] : ph[29:24];
		m = sine ? bt_qsin(i) : 15'd32767;
		wave = ph[31] ? -$signed({ 1'b0, m }) : $signed({ 1'b0, m });
	end
endfunction

reg signed [15:0] w_l = 16'sd0, w_r = 16'sd0;
reg signed [24:0] g_l = 25'sd0, g_r = 25'sd0;
always @(posedge clk) begin
	w_l <= c[0] ? wave(ph_l, c[2]) : 16'sd0;
	w_r <= c[1] ? wave(ph_r, c[2]) : 16'sd0;
	g_l <= w_l * $signed({ 1'b0, c[15:8] });
	g_r <= w_r * $signed({ 1'b0, c[15:8] });
	tape_out <= c[6] & ph_l[31];
end

// ---------------- tape ----------------
reg [2:0] tin = 3'b000;
always @(posedge clk) tin <= { tin[1:0], tape_in };
wire signed [15:0] tape_snd;
kg_tape_monitor #(.CLK_HZ(CLK_HZ)) tape_mon (
	.clk(clk), .mode(c[5:4]), .tape_in(tin[2]), .tape_out(tape_out), .in_on(), .out_on(), .sample(tape_snd)
);

// ---------------- the mix ----------------
function signed [15:0] sat(input signed [18:0] v);
	sat = (v > 19'sd32767) ? 16'sh7FFF : (v < -19'sd32768) ? 16'sh8000 : v[15:0];
endfunction
wire signed [15:0] a_l = c[3] ? adc_l : 16'sd0;
wire signed [15:0] a_r = c[3] ? adc_r : 16'sd0;
always @(posedge clk) begin
	out_l <= sat({ {3{g_l[23]}}, g_l[23:8] } + { {3{a_l[15]}}, a_l } + { {3{tape_snd[15]}}, tape_snd });
	out_r <= sat({ {3{g_r[23]}}, g_r[23:8] } + { {3{a_r[15]}}, a_r } + { {3{tape_snd[15]}}, tape_snd });
end

// ---------------- the 48 kHz samples for the CPU ----------------
localparam integer TICK = CLK_HZ / 48000;
reg [9:0] tick = 10'd0;
always @(posedge clk) begin
	if (tick == TICK - 1) begin
		tick <= 10'd0;
		smp_adc_l <= adc_l;
		smp_adc_r <= adc_r;
		smp_mix_l <= out_l;
		smp_mix_r <= out_r;
		smp_t <= ~smp_t;
	end else
		tick <= tick + 10'd1;
end

endmodule
