`timescale 1ns / 1ps
`default_nettype none
// The SDRAM tester bt_sdram against a model of the MT48LC16M16A2 with one defect (DEF) made on purpose, a small
// memory (2^RB rows a bank). The model takes the commands at the rise of SDR_CLK (= the fall of the tester clock, as
// the ODDR2 of CLKFX180 in the top) and checks the timing of each (tRCD, tRP, tRAS, tRC, tRFC, tWR, tMRD, the
// refresh: no gap over 8 x 7.8 us); a read gives its data tAC after the edge CL - 1 and holds it tOH after the edge CL.
// The bus has the two sides of tb_sram (an undriven side keeps its value).
//   DEF 0 none, 1 DQ K stuck at 1, 2 DQ K open, 3 DQ J, K shorted, 4 A J open (the chip sees 0), 5 A J, K shorted,
//       6 BA J open, 7 UDQM stuck high (the high byte never written nor read)
// T: the tester clock period (6.0 = 166.7 MHz, 10.0 = 100 MHz).
module tb_sdram;
parameter integer DEF = 0;
parameter integer J = 3;
parameter integer K = 6;
parameter integer RB = 3;                  // row bits modelled (the address lines checked: column 9, row RB, bank 2)
parameter real T = 6.0;
parameter integer CL = 3;
parameter integer RDLY = 7;

reg clk_cpu = 0, clk = 0;
always #6.667 clk_cpu = ~clk_cpu;
always #(T / 2) clk = ~clk;

reg we = 0;
reg [2:0] a = 0;
reg [31:0] wd = 0;
wire [31:0] rd;
reg locked = 0;
wire cs_n, ras_n, cas_n, we_n;
wire [1:0] ba, dqm;
wire [12:0] sa;
wire [15:0] dq_o, dq_oe_n;
reg [15:0] f_side = 16'h0, c_side = 16'h0;

bt_sdram dut (.clk_cpu(clk_cpu), .we(we), .a(a), .wd(wd), .rd(rd), .dcm_progen(), .dcm_progdata(), .dcm_rst(),
	.dcm_progdone(1'b1), .dcm_locked(locked), .clk(clk), .SDR_CS_N(cs_n), .SDR_BA(ba), .SDR_A(sa), .SDR_RAS_N(ras_n),
	.SDR_CAS_N(cas_n), .SDR_WE_N(we_n), .SDR_DQM(dqm), .DQ_I(f_side), .DQ_O(dq_o), .DQ_OE_N(dq_oe_n));

// ---------------- the chip ----------------
localparam integer MW = 2 + RB + 9;
reg [15:0] mem [0:(1 << MW) - 1];
integer i;
initial for (i = 0; i < (1 << MW); i = i + 1) mem[i] = $random;

// the pins as the chip sees them (defects of the address and control lines)
reg [12:0] ca;
reg [1:0] cba, cdqm;
always @* begin
	ca = sa; cba = ba; cdqm = dqm;
	if (DEF == 4) ca[J] = 1'b0;
	if (DEF == 5) begin ca[J] = sa[J] & sa[K]; ca[K] = sa[J] & sa[K]; end
	if (DEF == 6) cba[J] = 1'b0;
	if (DEF == 7) cdqm[1] = 1'b1;
end

reg [12:0] open_row [0:3];
reg [3:0] is_open = 0;
realtime t_act [0:3], t_pre [0:3], t_wr [0:3];
realtime t_ref = 0, t_mrd = 0, t_ref_last = 0, max_gap = 0;
integer errs = 0, nref = 0, ncmd = 0;
reg [15:0] rdat;
reg rdrive_lo = 0, rdrive_hi = 0;
initial for (i = 0; i < 4; i = i + 1) begin t_act[i] = -1000; t_pre[i] = -1000; t_wr[i] = -1000; end

task terr(input [8*40-1:0] what);
	begin errs = errs + 1; if (errs < 10) $display("%t timing: %0s", $realtime, what); end
endtask

// a read: the data out tAC after the edge CL - 1, off tOH after the edge CL (each byte whose DQM is low then); a queue
// of the reads by SDRAM edges
reg [3:0] rq_v = 0;
reg [15:0] rq_d [0:3];
reg rd_new = 0;
reg [15:0] rd_newd;
always @(negedge clk) begin : rq_proc
	integer q;
	#0.01;
	for (q = 0; q < 3; q = q + 1) begin rq_v[q] = rq_v[q+1]; rq_d[q] = rq_d[q+1]; end
	rq_v[3] = 0;
	if (rd_new) begin rq_v[CL-1] = 1; rq_d[CL-1] = rd_newd; rd_new = 0; end
	if (rq_v[0]) begin
		rdat <= #(5.4) rq_d[0];
		rdrive_lo <= #(5.4) !cdqm[0];
		rdrive_hi <= #(5.4) !cdqm[1];
		rdrive_lo <= #(T + 2.7) 1'b0;
		rdrive_hi <= #(T + 2.7) 1'b0;
	end
end
task do_read(input [MW-1:0] idx);
	begin rd_new = 1; rd_newd = mem[idx]; end
endtask

always @(negedge clk) begin
	if (!cs_n) begin
		case ({ ras_n, cas_n, we_n })
		3'b011: begin // ACTIVE
			ncmd = ncmd + 1;
			if (is_open[cba]) terr("ACT to an open bank");
			if ($realtime - t_pre[cba] < 18) terr("tRP");
			if ($realtime - t_act[cba] < 60) terr("tRC");
			if ($realtime - t_ref < 66) terr("tRFC (ACT)");
			is_open[cba] = 1; open_row[cba] = ca; t_act[cba] = $realtime;
		end
		3'b101, 3'b100: begin // READ, WRITE
			ncmd = ncmd + 1;
			if (!is_open[cba]) terr("READ/WRITE to a closed bank");
			if ($realtime - t_act[cba] < 18) terr("tRCD");
			if (ca[10]) terr("auto precharge");
			if (!we_n) begin
				if (!cdqm[0]) mem[{ cba, open_row[cba][RB-1:0], ca[8:0] }][7:0] = c_side[7:0];
				if (!cdqm[1]) mem[{ cba, open_row[cba][RB-1:0], ca[8:0] }][15:8] = c_side[15:8];
				t_wr[cba] = $realtime;
			end else do_read({ cba, open_row[cba][RB-1:0], ca[8:0] });
		end
		3'b010: begin // PRECHARGE
			if (ca[10]) begin
				for (i = 0; i < 4; i = i + 1) begin
					if (is_open[i] && $realtime - t_act[i] < 42) terr("tRAS (all)");
					is_open[i] = 0; t_pre[i] = $realtime;
				end
			end else begin
				if ($realtime - t_act[cba] < 42) terr("tRAS");
				if ($realtime - t_wr[cba] < 12) terr("tWR");
				is_open[cba] = 0; t_pre[cba] = $realtime;
			end
		end
		3'b001: begin // AUTO REFRESH
			if (is_open != 0) terr("REF with an open bank");
			for (i = 0; i < 4; i = i + 1) if ($realtime - t_pre[i] < 18) terr("tRP (REF)");
			if ($realtime - t_ref < 66) terr("tRFC");
			if (t_ref_last > 0 && $realtime - t_ref_last > max_gap) max_gap = $realtime - t_ref_last;
			t_ref = $realtime; t_ref_last = $realtime; nref = nref + 1;
		end
		3'b000: begin // MODE
			if (ca[6:4] != CL || ca[2:0] != 3'b000) terr("MODE: CL / burst");
			t_mrd = $realtime;
		end
		default: ;
		endcase
	end
end

// the bus: the FPGA side and the chip side
wire f_drv = (dq_oe_n == 16'h0000);
reg [15:0] fv, cv, fd, cd;
integer b;
always @* begin
	fv = f_side; cv = c_side; fd = 0; cd = 0;
	if (f_drv) begin fv = dq_o; fd = 16'hFFFF; end
	if (rdrive_lo) begin cv[7:0] = rdat[7:0]; cd[7:0] = 8'hFF; end
	if (rdrive_hi) begin cv[15:8] = rdat[15:8]; cd[15:8] = 8'hFF; end
	if (DEF == 1) begin cv[K] = 1'b1; cd[K] = 1'b1; end
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
always @(fv or cv) begin #0.5; f_side = fv; c_side = cv; end

// ---------------- the CPU side ----------------
task wreg(input [2:0] ad, input [31:0] v);
	begin @(posedge clk_cpu); a <= ad; wd <= v; we <= 1; @(posedge clk_cpu); we <= 0; end
endtask
task rreg(input [2:0] ad, output [31:0] v);
	begin @(posedge clk_cpu); a <= ad; @(posedge clk_cpu); @(negedge clk_cpu); v = rd; end
endtask
task wait_idle;
	reg [31:0] s;
	begin s = 1; while (s[0]) rreg(3'd0, s); end
endtask
task dwr(input [23:0] ad, input [15:0] v, input [1:0] ln);
	begin wreg(3'd1, ad); wreg(3'd2, v); wreg(3'd0, 32'd3 | (ln << 9)); wait_idle; end
endtask
task drd(input [23:0] ad, output [15:0] v);
	reg [31:0] r;
	begin wreg(3'd1, ad); wreg(3'd0, 32'd2); wait_idle; rreg(3'd2, r); v = r[15:0]; end
endtask
task elem(input dn, input r, input w, input [1:0] rp, input [1:0] wp);
	begin wreg(3'd0, 32'd1 | (dn << 2) | (r << 3) | (w << 4) | (rp << 5) | (wp << 7)); wait_idle; end
endtask

// the address bits of the model: column 0..8, row 9..9+RB-1, bank 22, 23
function [23:0] abit(input integer n);
	abit = n < 9 + RB ? (24'd1 << n) : (24'd1 << (22 + n - 9 - RB));
endfunction
localparam integer NA = 9 + RB + 2;

integer fails = 0;
reg [15:0] v, bad_d;
reg [23:0] bad_a;
reg [1:0] bad_lane;
reg [31:0] r, e, m, fa, fdat, st;
integer k, n;

initial begin
	#100 locked = 1;
	repeat (8) @(posedge clk_cpu);
	wreg(3'd6, CL | (RDLY << 4));
	wreg(3'd0, 32'h20000);   // init
	wait_idle;
	rreg(3'd0, st);
	if (!st[3]) begin $display("FAIL: not inited"); $finish; end

	// data lines
	bad_d = 0;
	for (k = 0; k < 16; k = k + 1) begin
		dwr(0, 16'd1 << k, 2'b11); dwr(1, ~(16'd1 << k), 2'b11); drd(0, v); bad_d = bad_d | (v ^ (16'd1 << k));
		dwr(0, ~(16'd1 << k), 2'b11); dwr(1, 16'd1 << k, 2'b11); drd(0, v); bad_d = bad_d | (v ^ ~(16'd1 << k));
	end
	// address lines (those of the model)
	bad_a = 0;
	for (k = -1; k < NA; k = k + 1) begin
		dwr(0, 16'h0000, 2'b11);
		for (n = 0; n < NA; n = n + 1) dwr(abit(n), 16'h0000, 2'b11);
		dwr(k < 0 ? 0 : abit(k), 16'hFFFF, 2'b11);
		drd(0, v); if (k >= 0 && v != 16'h0000) bad_a = bad_a | (1 << k);
		for (n = 0; n < NA; n = n + 1) begin
			drd(abit(n), v);
			if (n != k && v != 16'h0000) bad_a = bad_a | (1 << n) | (k >= 0 ? (1 << k) : 0);
			if (n == k && v != 16'hFFFF) bad_a = bad_a | (1 << n);
		end
	end
	// byte lanes
	bad_lane = 0;
	dwr(5, 16'h1234, 2'b11); dwr(5, 16'h00AB, 2'b01); drd(5, v); if (v != 16'h12AB) bad_lane = bad_lane | 1;
	dwr(5, 16'hCD00, 2'b10); drd(5, v); if (v != 16'hCDAB) bad_lane = bad_lane | 2;

	// march C- (0000), the LFSR, the address pattern over the rows of bank 0 of the model
	wreg(3'd5, (1 << (9 + RB)) - 1);
	wreg(3'd0, 32'h10000); wait_idle;
	wreg(3'd3, 32'h0000);
	elem(0, 0, 1, 0, 0); elem(0, 1, 1, 0, 1); elem(0, 1, 1, 1, 0); elem(1, 1, 1, 0, 1); elem(1, 1, 1, 1, 0); elem(1, 1, 0, 0, 0);
	wreg(3'd4, 32'h1234567);
	elem(0, 0, 1, 0, 2); elem(0, 1, 0, 2, 0);
	elem(0, 0, 1, 0, 3); elem(0, 1, 0, 3, 0);
	rreg(3'd3, e); rreg(3'd4, m); rreg(3'd5, fa); rreg(3'd6, fdat);

	$display("DEF %0d J %0d K %0d T %0.1f: data %04x, address %06x, lanes %0d, march errors %0d mask %04x first %06x got %04x exp %04x; refresh %0d, max gap %0.0f ns, timing errors %0d",
		DEF, J, K, T, bad_d, bad_a, bad_lane, e, m, fa[23:0], fdat[31:16], fdat[15:0], nref, max_gap, errs);
	if (errs) fails = 1;
	if (max_gap > 8 * 7800) fails = 1;
	case (DEF)
		0: if (bad_d || bad_a || bad_lane || e) fails = 1;
		1: if (bad_d != (16'd1 << K) || !e || m != (16'd1 << K)) fails = 1;
		2: if (bad_d != (16'd1 << K) || !e) fails = 1;
		3: if (bad_d != ((16'd1 << J) | (16'd1 << K)) || !e) fails = 1;
		4: if ((bad_a & ((1 << J) | (1 << (9 + J)))) == 0) fails = 1;
		5: if ((bad_a & ((1 << J) | (1 << K) | (1 << (9 + J)) | (1 << (9 + K)))) == 0) fails = 1;
		6: if ((bad_a & (1 << (9 + RB + J))) == 0) fails = 1;
		7: if (bad_d[15:8] == 0 || !e) fails = 1;
	endcase
	if (fails) $display("FAIL"); else $display("PASS");
	$finish;
end

initial begin #100000000; $display("FAIL: timeout"); $finish; end
endmodule
