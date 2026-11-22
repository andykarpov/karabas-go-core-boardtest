`timescale 1ns / 1ps
`default_nettype none
// The SRAM tester bt_sram against a model of the two 2M x 8 chips with one defect (DEF) made on purpose, on a small
// memory (2^AW halfwords). The bus has two sides: the FPGA one and the chips one, joined bit by bit unless the
// line is open; an undriven side keeps the last value driven on it (as a real floating line does), so an open line
// is not seen by a write and a read back of the same value.
//   DEF 0 none, 1 MD K stuck at 1 at the chips, 2 MD K open, 3 MD J, K shorted (wired AND), 4 MA J, K shorted,
//       5 MA J open (the chips see 0), 6 /WE of the high byte dead, 7 /OE of the low byte dead
// The tb runs what the program does: the data line check (direct accesses), the address line check, march C-
// (background 0000 and 5555), the LFSR fill / verify, the byte lanes; it prints what each found and fails when a
// defect is not seen or a good memory is.
module tb_sram;
parameter integer DEF = 0;
parameter integer J = 3;
parameter integer K = 6;
parameter integer AW = 10;

reg clk = 0;
always #6.667 clk = ~clk;

reg we = 0;
reg [2:0] a = 0;
reg [31:0] wd = 0;
wire [31:0] rd;
wire [20:0] MA;
wire [15:0] md_o, md_oe_n;
wire [1:0] mwr_n, mrd_n;
reg [15:0] f_side = 16'h0, c_side = 16'h0;   // the two sides of MD

bt_sram dut (.clk(clk), .we(we), .a(a), .wd(wd), .rd(rd), .MA(MA), .MD_I(f_side), .MD_O(md_o), .MD_OE_N(md_oe_n),
	.MWR_N(mwr_n), .MRD_N(mrd_n));

// ---------------- the chips ----------------
reg [7:0] lo [0:(1<<AW)-1];
reg [7:0] hi [0:(1<<AW)-1];
integer i;
initial for (i = 0; i < (1 << AW); i = i + 1) begin lo[i] = $random; hi[i] = $random; end

reg [20:0] ca;
always @* begin
	ca = MA;
	if (DEF == 4) begin ca[J] = MA[J] & MA[K]; ca[K] = MA[J] & MA[K]; end
	if (DEF == 5) ca[J] = 1'b0;
end
wire [AW-1:0] cidx = ca[AW-1:0];
wire [1:0] wr_n = (DEF == 6) ? (mwr_n | 2'b10) : mwr_n;
wire [1:0] oe_n = (DEF == 7) ? (mrd_n | 2'b01) : mrd_n;

// the drive of each side: the FPGA on its side, the chips (reading) on theirs
wire f_drv = (md_oe_n == 16'h0000);
wire [15:0] c_dat = { hi[cidx], lo[cidx] };
reg [15:0] fv, cv;
reg [15:0] fd, cd;   // driven on the side (bit mask)
integer b;
always @* begin
	fv = f_side; cv = c_side; fd = 16'h0; cd = 16'h0;
	// the drivers on their own side
	if (f_drv) begin fv = md_o; fd = 16'hFFFF; end
	if (!oe_n[0] && wr_n[0]) begin cv[7:0] = c_dat[7:0]; cd[7:0] = 8'hFF; end
	if (!oe_n[1] && wr_n[1]) begin cv[15:8] = c_dat[15:8]; cd[15:8] = 8'hFF; end
	if (DEF == 1) begin cv[K] = 1'b1; cd[K] = 1'b1; end
	// joined lines: one value on both sides
	for (b = 0; b < 16; b = b + 1)
		if (!(DEF == 2 && b == K)) begin
			if (fd[b]) cv[b] = fv[b];
			else if (cd[b]) fv[b] = cv[b];
		end
	if (DEF == 3) begin
		fv[J] = fv[J] & fv[K]; fv[K] = fv[J];
		cv[J] = cv[J] & cv[K]; cv[K] = cv[J];
	end
end
always @(posedge clk or negedge clk) begin f_side <= fv; c_side <= cv; end

// a write: the chip takes the data of its side at the rise of /WE
always @(posedge wr_n[0]) lo[cidx] <= c_side[7:0];
always @(posedge wr_n[1]) hi[cidx] <= c_side[15:8];

// ---------------- the CPU side ----------------
task wreg(input [2:0] ad, input [31:0] v);
	begin @(posedge clk); a <= ad; wd <= v; we <= 1; @(posedge clk); we <= 0; end
endtask
task rreg(input [2:0] ad, output [31:0] v);
	begin @(posedge clk); a <= ad; @(posedge clk); @(negedge clk); v = rd; end
endtask
task wait_idle;
	reg [31:0] s;
	begin s = 1; while (s[0]) rreg(3'd0, s); end
endtask
task dwr(input [20:0] ad, input [15:0] v, input [1:0] ln);
	begin wreg(3'd1, ad); wreg(3'd2, v); wreg(3'd0, 32'd3 | (ln << 9)); wait_idle; end
endtask
task drd(input [20:0] ad, output [15:0] v);
	reg [31:0] r;
	begin wreg(3'd1, ad); wreg(3'd0, 32'd2); wait_idle; rreg(3'd2, r); v = r[15:0]; end
endtask
// an element: down, read, write, rpat, wpat
task elem(input dn, input r, input w, input [1:0] rp, input [1:0] wp);
	begin wreg(3'd0, 32'd1 | (dn << 2) | (r << 3) | (w << 4) | (rp << 5) | (wp << 7)); wait_idle; end
endtask

integer errs_all = 0;
reg [15:0] v, bad_d, bad_lane;
reg [20:0] bad_a;
reg [31:0] r, e, m, fa, fdat;
integer k, n;

initial begin
	repeat (4) @(posedge clk);
	wreg(3'd5, (1 << AW) - 1);

	// data lines: 1 << k at 0, its complement at 1 (the bus driven the other way), read 0
	bad_d = 0;
	for (k = 0; k < 16; k = k + 1) begin
		dwr(0, 16'd1 << k, 2'b11); dwr(1, ~(16'd1 << k), 2'b11); drd(0, v); bad_d = bad_d | (v ^ (16'd1 << k));
		dwr(0, ~(16'd1 << k), 2'b11); dwr(1, 16'd1 << k, 2'b11); drd(0, v); bad_d = bad_d | (v ^ ~(16'd1 << k));
	end
	// address lines: 0 at 0 and at each 1 << k, then 0xFFFF at 1 << k: who else changed
	bad_a = 0;
	for (k = -1; k < AW; k = k + 1) begin
		dwr(0, 16'h0000, 2'b11);
		for (n = 0; n < AW; n = n + 1) dwr(1 << n, 16'h0000, 2'b11);
		dwr(k < 0 ? 0 : (1 << k), 16'hFFFF, 2'b11);
		drd(0, v); if (k >= 0 && v != 16'h0000) bad_a = bad_a | (1 << k);
		for (n = 0; n < AW; n = n + 1) begin
			drd(1 << n, v);
			if (n != k && v != 16'h0000) bad_a = bad_a | (1 << n) | (k >= 0 ? (1 << k) : 0);
			if (n == k && v != 16'hFFFF) bad_a = bad_a | (1 << n);
		end
	end
	// byte lanes: 0x1234, then 0xAB in the low byte only, 0xCD in the high one only
	bad_lane = 0;
	dwr(5, 16'h1234, 2'b11); dwr(5, 16'h00AB, 2'b01); drd(5, v); if (v != 16'h12AB) bad_lane = bad_lane | 1;
	dwr(5, 16'hCD00, 2'b10); drd(5, v); if (v != 16'hCDAB) bad_lane = bad_lane | 2;

	// march C- on background 0000 then 5555; then the LFSR fill / verify; then the address pattern
	wreg(3'd0, 32'h10000);
	for (n = 0; n < 2; n = n + 1) begin
		wreg(3'd3, n ? 32'h5555 : 32'h0000);
		elem(0, 0, 1, 0, 0); elem(0, 1, 1, 0, 1); elem(0, 1, 1, 1, 0); elem(1, 1, 1, 0, 1); elem(1, 1, 1, 1, 0); elem(1, 1, 0, 0, 0);
	end
	wreg(3'd4, 32'h1234567);
	elem(0, 0, 1, 0, 2); elem(0, 1, 0, 2, 0);
	elem(0, 0, 1, 0, 3); elem(0, 1, 0, 3, 0);
	rreg(3'd3, e); rreg(3'd4, m); rreg(3'd5, fa); rreg(3'd6, fdat);

	$display("DEF %0d J %0d K %0d: data lines %04x, address lines %06x, lanes %0d, march errors %0d mask %04x first %06x got %04x exp %04x",
		DEF, J, K, bad_d, bad_a, bad_lane, e, m, fa[20:0], fdat[31:16], fdat[15:0]);
	case (DEF)
		0: if (bad_d || bad_a || bad_lane || e) errs_all = 1;
		1: if (bad_d != (16'd1 << K) || !e || m != (16'd1 << K)) errs_all = 1;
		2: if (bad_d != (16'd1 << K) || !e) errs_all = 1;
		3: if (bad_d != ((16'd1 << J) | (16'd1 << K)) || !e) errs_all = 1;
		4: if ((bad_a & ((1 << J) | (1 << K))) == 0 || !e) errs_all = 1;
		5: if ((bad_a & (1 << J)) == 0 || !e) errs_all = 1;
		6: if (!bad_lane[1] || !e) errs_all = 1;
		7: if (bad_d[7:0] == 0 || !e) errs_all = 1;
	endcase
	if (errs_all) $display("FAIL"); else $display("PASS");
	$finish;
end

initial begin #200000000; $display("FAIL: timeout"); $finish; end
endmodule
