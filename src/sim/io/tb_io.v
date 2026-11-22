`timescale 1ns / 1ps
`default_nettype none
// tb_io: bt_audio (clk_sys) + bt_io (clk_cpu) with cables: generator L (a sine of 2 kHz) into the left ADC input,
// TAPE_OUT into TAPE_IN. Checks the zero crossings and peaks of the ADC, the tape edges, the DNA (the unisim model:
// it gives 1 for bit 56), a MIDI byte on the line, a frame of the CPU to the matrix. A few seconds (iverilog).
module tb_io;
reg clk_sys = 0, clk_cpu = 0;
always #12.222 clk_sys = ~clk_sys;   // 40.91 MHz
always #6.667 clk_cpu = ~clk_cpu;    // 75 MHz

wire [15:0] aud_ctl;
wire [31:0] inc_l, inc_r;
wire signed [15:0] out_l, out_r, s_adc_l, s_adc_r, s_mix_l, s_mix_r;
wire smp_t, tape_out, midi_tx;
wire [23:0] mcmd;
wire mwr;
reg we = 0, rd_go = 0;
reg [8:0] a = 0;
reg [31:0] wd = 0;
wire [31:0] rd;
reg [5:0] btns = 6'b100101;
// the cable: what goes out on the left comes back on the left ADC input (the ADC passthrough off)
bt_audio #(.CLK_HZ(40909090)) au (
	.clk(clk_sys), .ctl(aud_ctl), .inc_l(inc_l), .inc_r(inc_r), .adc_l(out_l), .adc_r(16'sd0), .tape_in(tape_out),
	.out_l(out_l), .out_r(out_r), .tape_out(tape_out),
	.smp_t(smp_t), .smp_adc_l(s_adc_l), .smp_adc_r(s_adc_r), .smp_mix_l(s_mix_l), .smp_mix_r(s_mix_r)
);
bt_io #(.CLK_HZ(75000000)) io (
	.clk(clk_cpu), .we(we), .a(a), .wd(wd), .rd(rd), .rd_go(rd_go), .btns(btns), .dvi_only(1'b1),
	.matrix_cmd(mcmd), .matrix_cmd_wr(mwr), .aud_ctl(aud_ctl), .aud_inc_l(inc_l), .aud_inc_r(inc_r),
	.smp_t(smp_t), .smp_adc_l(s_adc_l), .smp_adc_r(s_adc_r), .smp_mix_l(s_mix_l), .smp_mix_r(s_mix_r),
	.tape_in(tape_out), .midi_tx(midi_tx)
);

integer errors = 0;
task wr(input [8:0] aa, input [31:0] d);
	begin @(posedge clk_cpu); a <= aa; wd <= d; we <= 1; @(posedge clk_cpu); we <= 0; end
endtask
reg [31:0] v;
task rdr(input [8:0] aa);
	begin @(posedge clk_cpu); a <= aa; rd_go <= 1; @(posedge clk_cpu); rd_go <= 0; @(posedge clk_cpu); v = rd; end
endtask

// a frame of the CPU to the matrix
reg seen_cpu = 0;
always @(posedge clk_cpu) if (mwr && mcmd == 24'h610523) seen_cpu <= 1;

// the MIDI line: the first byte (8N1, 31250 baud = 32 us a bit)
reg [7:0] midi_byte = 0;
reg midi_got = 0;
integer k;
initial begin
	@(negedge midi_tx);
	#16000;                       // the middle of the start bit
	for (k = 0; k < 8; k = k + 1) begin #32000; midi_byte[k] = midi_tx; end
	midi_got = 1;
end

reg [31:0] zc0, tp0;
initial begin
	#1000;
	// generator L: sine, 2 kHz (inc = 2000 * 2^32 / 40909090), volume 255, TAPE_OUT from it; the ADC off in the mix
	wr(9'h005, 32'd209978);
	wr(9'h004, { 16'd0, 8'd255, 1'b0, 1'b1, 2'b00, 1'b0, 1'b1, 1'b0, 1'b1 });
	wr(9'h003, 24'h610523);
	wr(9'h00B, 32'h90);
	#2000000;                     // 2 ms to settle
	rdr(9'h008); zc0 = v;
	rdr(9'h00A); tp0 = v;
	rdr(9'h007);                  // clear the peaks
	#5000000;                     // 5 ms: 10 cycles of 2 kHz
	rdr(9'h008);
	$display("tb_io: %0d zero crossings in 5 ms (10 expected)", v - zc0);
	if (v - zc0 < 9 || v - zc0 > 11) errors = errors + 1;
	rdr(9'h00A);
	$display("tb_io: %0d tape edges in 5 ms (10 expected)", (v >> 8) - (tp0 >> 8));
	if ((v >> 8) - (tp0 >> 8) < 9 || (v >> 8) - (tp0 >> 8) > 11) errors = errors + 1;
	rdr(9'h007);
	$display("tb_io: ADC peak left %0d right %0d", v[15:0], v[31:16]);
	if (v[15:0] < 31000 || v[31:16] != 0) errors = errors + 1;
	rdr(9'h00C);
	$display("tb_io: %0d samples", v);
	if (v < 300 || v > 400) errors = errors + 1;      // 7 ms at 48 kHz = 336
	rdr(9'h002);
	if (v[5:0] != 6'b100101 || !v[8]) begin $display("tb_io: BTN %h", v); errors = errors + 1; end
	rdr(9'h001); zc0 = v;
	rdr(9'h000);
	$display("tb_io: DNA valid %0d %h%h", zc0[31], zc0[24:0], v);
	if (!zc0[31] || { zc0[24:0], v } != 57'h1123456789ABCDE) errors = errors + 1;
	if (!seen_cpu) begin $display("tb_io: no matrix frame"); errors = errors + 1; end
	wait (midi_got);
	$display("tb_io: MIDI byte %h", midi_byte);
	if (midi_byte != 8'h90) errors = errors + 1;
	$display("tb_io: %0d errors", errors);
	$finish;
end
endmodule
