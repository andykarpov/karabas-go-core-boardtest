`timescale 1ns / 1ps
`default_nettype none
/*
 bt_sdram.v

 The SDRAM tester of the board test (docs/PLAN.md Ф2). The SDRAM: 32 MB, 16 bit, 4 banks x 8192 rows x 512 columns
 (MT48LC16M16A2). A halfword address cur[23:0] = { bank, row, column }. Nothing else uses the SDRAM in this core.

 Clocks. The registers of the CPU are on clk_cpu (75 MHz), the tester on clk (CLKFX of a DCM_CLKGEN on clk_cpu, the
 top): its M / D are set by the CPU (FREQ) over the PROG port of the DCM (as the frequency keys of MiSTer MemTest and
 of the old sdram-test core), 100 .. 140 MHz. The SDRAM clock is CLKFX180 (an ODDR2 in the top): a command out of
 the IOB flip-flops at a rise of clk is taken by the SDRAM half a clock later. A read gives its data CL clocks after
 that plus tAC; MD is taken by IDDR2s at both edges of clk, the one RDLY half clocks after the READ is used (the CPU
 sets it: the program finds the window at each frequency; 7 suits the datasheet timing at CL 3 from 100 to 140 MHz).

 The CPU side crosses by a toggle (GO) and the busy flag (two flip-flops); the registers it wrote are steady while the
 tester runs, the results steady while it is idle (TS_SDR_* of the ucf: DATAPATHONLY).

 Operations (GO, as bt_sram):
   - init: the power up sequence (a wait of 200 us or more, PRECHARGE ALL, 8 AUTO REFRESH, MODE: CL, burst of 1);
     needed after every change of the clock.
   - a direct access of one halfword (ACTIVE, READ / WRITE with the DQM of the lanes, PRECHARGE);
   - an element over 0..LAST, up or down: a row is opened, then at each column an optional read compared with a
     pattern and an optional write: a read only or a write only element goes a column a clock, a read and a write one
     a column in CL + 3 clocks (the bus turned between them). Refresh: one owed each 768 clocks (7.7 us at 100 MHz),
     paid when a row is closed (a row of a read + write element is 18 us at 166 MHz: up to 4 owed, within the 8 the
     SDRAM allows to postpone).
 Patterns: 0 BG, 1 ~BG, 2 a 32 bit LFSR from SEED stepped at each address (up only), 3 the address
 (a[15:0] ^ a[23:16]).

 Registers (MMIO 0x80000a00.., a = word):
   00 W GO: bits 1:0 the operation (0 with bit 17: init; 1 element, 2 direct read, 3 direct write); element: bit 2 down,
        bit 3 read, bit 4 write, 6:5 the read pattern, 8:7 the write pattern; direct write: 10:9 the lanes; bit 16
        clears the results first
   04 W ADR (24 bits)   08 W DAT   0c W BG   10 W SEED   14 W LAST (16M - 1 at reset)
   18 W CFG: bits 1:0 CL (2 or 3), bits 7:4 RDLY: the read taken RDLY half clocks after the READ (2..15, 7 at reset)
   1c W FREQ: bits 7:0 M - 1, 15:8 D - 1: the DCM programmed to 75 MHz x M / D (the tester stops, init after)
   00 R STATUS: bit 0 busy, 1 the clock is being changed, 2 DCM locked, 3 inited
   04 R CUR (bits 23:16 are enough for a progress)   08 R DAT   0c R ERRORS   10 R MASK   14 R FIRST_ADR (bit 31 valid)
   18 R FIRST_DAT { read, expected }
*/
module bt_sdram (
	input wire clk_cpu,
	input wire we,
	input wire [2:0] a,
	input wire [31:0] wd,
	output reg [31:0] rd = 32'd0,          // a clock after the address

	// the DCM of the top
	output reg dcm_progen = 1'b0,
	output reg dcm_progdata = 1'b0,
	output reg dcm_rst = 1'b0,
	input wire dcm_progdone,
	input wire dcm_locked,

	input wire clk,                        // the tester clock (CLKFX)
	output wire SDR_CS_N,
	output wire [1:0] SDR_BA,
	output wire [12:0] SDR_A,
	output wire SDR_RAS_N,
	output wire SDR_CAS_N,
	output wire SDR_WE_N,
	output wire [1:0] SDR_DQM,
	input wire [15:0] DQ_I,
	output wire [15:0] DQ_O,
	output wire [15:0] DQ_OE_N
);

// ================================================================ the CPU side (clk_cpu)
reg [23:0] r_adr = 24'd0;
reg [15:0] r_dat = 16'd0;
reg [15:0] r_bg = 16'd0;
reg [31:0] r_seed = 32'h1;
reg [23:0] r_last = 24'hFFFFFF;
reg [1:0] r_cl = 2'd3;
reg [3:0] r_rdly = 4'd7;                   // half clocks
reg [17:0] r_go = 18'd0;
reg go_t = 1'b0;                           // toggles at each GO
reg busy_c = 1'b0;                         // set by GO, cleared when the tester is seen done

// results (the tester side), read by the CPU while idle
reg busy = 1'b0;
reg inited = 1'b0;
reg [23:0] cur = 24'd0;
reg [15:0] dread = 16'd0;
reg [31:0] errors = 32'd0;
reg [15:0] mask = 16'd0;
reg first_v = 1'b0;
reg [23:0] first_a = 24'd0;
reg [31:0] first_d = 32'd0;
reg done_t = 1'b0;                         // the tester toggles it when an operation is over

reg [2:0] done_s = 3'd0;
reg [1:0] lock_s = 2'd0;
reg prog_busy = 1'b0;

always @(posedge clk_cpu) begin
	done_s <= { done_s[1:0], done_t };
	lock_s <= { lock_s[0], dcm_locked };
	if (done_s[2] != done_s[1]) busy_c <= 1'b0;
	if (we) begin
		case (a)
			3'd0: if (!busy_c && !prog_busy) begin r_go <= wd[17:0]; go_t <= ~go_t; busy_c <= 1'b1; end
			3'd1: r_adr <= wd[23:0];
			3'd2: r_dat <= wd[15:0];
			3'd3: r_bg <= wd[15:0];
			3'd4: r_seed <= wd;
			3'd5: r_last <= wd[23:0];
			3'd6: begin r_cl <= (wd[1:0] == 2'd2) ? 2'd2 : 2'd3; r_rdly <= wd[7:4]; end
			default: ;
		endcase
	end
	case (a)
		3'd0: rd <= { 28'd0, inited, lock_s[1], prog_busy, busy_c };
		3'd1: rd <= { 8'd0, cur };
		3'd2: rd <= { 16'd0, dread };
		3'd3: rd <= errors;
		3'd4: rd <= { 16'd0, mask };
		3'd5: rd <= { first_v, 7'd0, first_a };
		default: rd <= first_d;
	endcase
end

// ---------------- the DCM program (PROGCLK = clk_cpu): LoadD (1 0, D-1 LSB first), LoadM (1 1, M-1), GO; then
// PROGDONE and LOCKED. The tester is held in reset meanwhile (dcm_locked low).
localparam P_IDLE = 3'd0, P_LOAD = 3'd1, P_GAP = 3'd2, P_GO = 3'd3, P_WAIT = 3'd4;
reg [2:0] pst = P_IDLE;
reg [3:0] pbit = 4'd0;
reg [9:0] pword = 10'd0;
reg [7:0] pgap = 8'd0;
reg pnext_m = 1'b0;
reg [7:0] p_m = 8'd0;

always @(posedge clk_cpu) begin
	dcm_progen <= 1'b0;
	dcm_progdata <= 1'b0;
	dcm_rst <= 1'b0;
	case (pst)
	P_IDLE:
		if (we && a == 3'd7 && !busy_c) begin
			prog_busy <= 1'b1;
			pword <= { wd[15:8], 2'b01 };   // shifted out LSB first: 1, 0, then D - 1
			p_m <= wd[7:0];
			pbit <= 4'd0;
			pnext_m <= 1'b1;
			pst <= P_LOAD;
		end
	P_LOAD: begin
		dcm_progen <= 1'b1;
		dcm_progdata <= pword[0];
		pword <= { 1'b0, pword[9:1] };
		pbit <= pbit + 4'd1;
		if (pbit == 4'd9) begin
			pgap <= 8'd4;
			pst <= P_GAP;
		end
	end
	P_GAP:
		if (pgap != 8'd0) pgap <= pgap - 8'd1;
		else if (pnext_m) begin
			pnext_m <= 1'b0;
			pword <= { p_m, 2'b11 };
			pbit <= 4'd0;
			pst <= P_LOAD;
		end else pst <= P_GO;
	P_GO: begin
		dcm_progen <= 1'b1;                 // GO: PROGEN one cycle, PROGDATA 0
		pgap <= 8'd255;
		pst <= P_WAIT;
	end
	P_WAIT:
		if (pgap != 8'd0) pgap <= pgap - 8'd1;
		else if (dcm_progdone && lock_s[1]) begin
			prog_busy <= 1'b0;
			pst <= P_IDLE;
		end
	default: pst <= P_IDLE;
	endcase
end

// ================================================================ the tester (clk)
reg [2:0] rst_s = 3'b111;
always @(posedge clk or negedge dcm_locked)
	if (!dcm_locked) rst_s <= 3'b111;
	else rst_s <= { rst_s[1:0], 1'b0 };
wire rst = rst_s[2];

reg [2:0] go_s = 3'd0;
always @(posedge clk) go_s <= { go_s[1:0], go_t };
reg go_take = 1'b0;
reg go_pend = 1'b0;                        // a GO seen, not yet taken (the tester may be refreshing)
always @(posedge clk)
	if (go_s[2] != go_s[1]) go_pend <= 1'b1;
	else if (go_take) go_pend <= 1'b0;
wire go_new = go_pend & ~go_take;

// pins
// the state machine sets these; the pins take them a clock later from IOB flip-flops (no clock enable: the IOBs are
// far, a CE from the state would not meet 6 ns), DQ through ODDR2s (the IDDR2s of the reads share the IOB)
reg [2:0] cmd = 3'b111;                    // RAS, CAS, WE
reg [12:0] sa = 13'd0;
reg [1:0] sba = 2'd0;
reg [1:0] dqm = 2'b11;
reg [15:0] dq_out = 16'd0;
(* equivalent_register_removal = "no" *) reg [15:0] dq_oe_n = 16'hFFFF;
(* IOB = "TRUE" *) reg [2:0] cmd_q = 3'b111;
(* IOB = "TRUE" *) reg [12:0] sa_q = 13'd0;
(* IOB = "TRUE" *) reg [1:0] sba_q = 2'd0;
(* IOB = "TRUE" *) reg [1:0] dqm_q = 2'b11;
always @(posedge clk) begin cmd_q <= cmd; sa_q <= sa; sba_q <= sba; dqm_q <= dqm; end
assign SDR_CS_N = 1'b0;
assign { SDR_RAS_N, SDR_CAS_N, SDR_WE_N } = cmd_q;
assign SDR_A = sa_q;
assign SDR_BA = sba_q;
assign SDR_DQM = dqm_q;

localparam [2:0] C_NOP = 3'b111, C_ACT = 3'b011, C_READ = 3'b101, C_WRITE = 3'b100, C_PRE = 3'b010, C_REF = 3'b001,
	C_MRS = 3'b000;
// timing at 166.7 MHz (6 ns) and below (the table goes to 140 MHz): tRCD 3, tRP 3, tRAS 7, tWR 2, tRFC 11 clocks (+1 each to be safe)
localparam [3:0] T_RCD = 4'd4, T_RP = 4'd4, T_WR = 4'd3, T_RFC = 4'd12;
localparam [3:0] T_RAS = 4'd8;
localparam [9:0] REF_INT = 10'd767;

// the operation
reg el = 1'b0, dn = 1'b0, do_rd = 1'b0, do_wr = 1'b0, dir = 1'b0;
reg [1:0] rpat = 2'd0, wpat = 2'd0, lanes = 2'b11;
reg [31:0] lfsr = 32'h1;
reg [1:0] cl = 2'd3;
reg [3:0] rdly = 4'd7;                     // the read taken RDLY half clocks after the READ
reg [2:0] rk = 3'd4;                       // the clock it is seen at: (RDLY + 1) / 2
reg rfall = 1'b1;                          // taken at a fall of clk (RDLY odd)
reg [15:0] bg = 16'd0;
reg [23:0] last = 24'd0;

function [31:0] lfsr_step(input [31:0] s);
	lfsr_step = s[0] ? ((s >> 1) ^ 32'h80200003) : (s >> 1);
endfunction
function [15:0] pat(input [1:0] p, input [23:0] ad, input [31:0] l);
	case (p)
		2'd0: pat = bg;
		2'd1: pat = ~bg;
		2'd2: pat = l[15:0];
		default: pat = ad[15:0] ^ { 8'd0, ad[23:16] };
	endcase
endfunction

// refresh owed
reg [9:0] ref_cnt = 10'd0;
reg [3:0] ref_owed = 4'd0;
reg ref_pay = 1'b0;

// the read pipeline: an entry per clock since a READ; taken at stage rdly
reg [8:0] p_v = 9'd0;
reg [8:0] p_dir = 9'd0;                    // the entry is a direct read
reg [15:0] p_exp [0:8];
reg [23:0] p_adr [0:8];
integer pi;
initial for (pi = 0; pi < 9; pi = pi + 1) begin p_exp[pi] = 16'd0; p_adr[pi] = 24'd0; end
reg push = 1'b0;
reg [15:0] push_exp = 16'd0;
reg [23:0] push_adr = 24'd0;
// the stage taken: registered once more for the compare
reg c_v = 1'b0, c_dir = 1'b0;
reg [15:0] c_exp = 16'd0;
reg [23:0] c_adr = 24'd0;

localparam [3:0] S_RESET = 4'd0, S_WAIT = 4'd1, S_IPRE = 4'd2, S_IREF = 4'd3, S_IMRS = 4'd4, S_IDLE = 4'd5,
	S_ACT = 4'd6, S_COL = 4'd7, S_RW = 4'd8, S_DRAIN = 4'd9, S_PRE = 4'd10, S_REF = 4'd11, S_DONE = 4'd12;
reg [3:0] st = S_RESET;
reg [3:0] tcnt = 4'd0;                     // a wait
reg tz = 1'b1;                             // tcnt is 0 (a flip-flop: the 6 ns paths)
reg [15:0] iwait = 16'd0;                  // the wait of the power up
reg [3:0] ras_cnt = 4'd0;                  // clocks since ACTIVE
reg [3:0] wr_cnt = 4'd0;                   // clocks since the last WRITE
reg [3:0] nref = 4'd0;
reg [3:0] rw_n = 4'd0;
reg row_end = 1'b0;                        // the column now is the last one of its row (or of the element)
reg op_end = 1'b0;                         // the address now is the last one of the operation

wire [23:0] nxt = dn ? cur - 24'd1 : cur + 24'd1;
wire el_last = dn ? (cur == 24'd0) : (cur == last);
wire col_last = dn ? (cur[8:0] == 9'd0) : (cur[8:0] == 9'h1FF);

always @(posedge clk) begin
	// defaults: a NOP, the DQ IOB takes the bus
	cmd <= C_NOP;
	go_take <= 1'b0;
	push <= 1'b0;
	if (ras_cnt != 4'hF) ras_cnt <= ras_cnt + 4'd1;
	if (wr_cnt != 4'hF) wr_cnt <= wr_cnt + 4'd1;

	// refresh owed
	if (ref_cnt == REF_INT) begin
		ref_cnt <= 10'd0;
		if (ref_owed != 4'hF) ref_owed <= ref_owed + (ref_pay ? 4'd0 : 4'd1);
	end else begin
		ref_cnt <= ref_cnt + 10'd1;
		if (ref_pay && ref_owed != 4'd0) ref_owed <= ref_owed - 4'd1;
	end
	ref_pay <= 1'b0;

	// the read pipeline
	p_v <= { p_v[7:0], push };
	p_dir <= { p_dir[7:0], push & ~el };
	p_exp[0] <= push_exp;
	p_adr[0] <= push_adr;
	for (pi = 1; pi < 9; pi = pi + 1) begin p_exp[pi] <= p_exp[pi-1]; p_adr[pi] <= p_adr[pi-1]; end
	// the stage rk of the pipeline is the READ that left the pins rk clocks ago: the IDDR2 has its data at this edge
	c_v <= p_v[rk];
	c_dir <= p_dir[rk];
	c_exp <= p_exp[rk];
	c_adr <= p_adr[rk];
	if (c_v) begin
		if (c_dir) dread <= dq_in_d;
		else if (dq_in_d != c_exp) begin
			if (errors != 32'hFFFFFFFF) errors <= errors + 32'd1;
			mask <= mask | (dq_in_d ^ c_exp);
			if (!first_v) begin
				first_v <= 1'b1;
				first_a <= c_adr;
				first_d <= { dq_in_d, c_exp };
			end
		end
	end

	if (rst) begin
		st <= S_RESET;
		busy <= 1'b0;
		inited <= 1'b0;
		dq_oe_n <= 16'hFFFF;
		dqm <= 2'b11;
		p_v <= 9'd0;
	end else case (st)
	S_RESET: begin
		inited <= 1'b0;
		st <= S_IDLE;
	end

	// init: 2^15 clocks (197 us at 166 MHz), PRECHARGE ALL, 8 x AUTO REFRESH, MODE
	S_WAIT: begin
		if (iwait == 16'd0) begin
			cmd <= C_PRE;
			sa[10] <= 1'b1;
			begin tcnt <= T_RP; tz <= (T_RP) == 4'd0; end
			nref <= 4'd8;
			st <= S_IPRE;
		end else iwait <= iwait - 16'd1;
	end
	S_IPRE, S_IREF: begin
		if (!tz) begin tcnt <= tcnt - 4'd1; tz <= tcnt == 4'd1; end
		else if (nref != 4'd0) begin
			cmd <= C_REF;
			nref <= nref - 4'd1;
			begin tcnt <= T_RFC; tz <= (T_RFC) == 4'd0; end
			st <= S_IREF;
		end else begin
			cmd <= C_MRS;
			sba <= 2'b00;
			sa <= { 3'b000, 1'b0, 2'b00, 1'b0, cl, 1'b0, 3'b000 };   // single write, CL, sequential, burst of 1
			begin tcnt <= 4'd3; tz <= (4'd3) == 4'd0; end
			st <= S_IMRS;
		end
	end
	S_IMRS: begin
		if (!tz) begin tcnt <= tcnt - 4'd1; tz <= tcnt == 4'd1; end
		else begin
			inited <= 1'b1;
			ref_cnt <= 10'd0;
			ref_owed <= 4'd0;
			busy <= 1'b0;
			done_t <= ~done_t;
			st <= S_IDLE;
		end
	end

	S_IDLE: begin
		dq_oe_n <= 16'hFFFF;
		dqm <= 2'b11;
		if (ref_owed != 4'd0 && inited) begin
			// pay what is owed while idle (all banks are closed here)
			cmd <= C_REF;
			ref_pay <= 1'b1;
			begin tcnt <= T_RFC; tz <= (T_RFC) == 4'd0; end
			st <= S_REF;
		end else if (go_new) begin
			go_take <= 1'b1;
			busy <= 1'b1;
			if (r_go[16]) begin
				errors <= 32'd0;
				mask <= 16'd0;
				first_v <= 1'b0;
			end
			el <= r_go[1:0] == 2'd1;
			dn <= r_go[1:0] == 2'd1 && r_go[2];
			do_rd <= r_go[1:0] == 2'd1 ? r_go[3] : r_go[1:0] == 2'd2;
			do_wr <= r_go[1:0] == 2'd1 ? r_go[4] : r_go[1:0] == 2'd3;
			rpat <= r_go[6:5];
			wpat <= r_go[8:7];
			lanes <= r_go[10:9];
			bg <= r_bg;
			last <= r_last;
			lfsr <= r_seed;
			cl <= r_cl;
			rdly <= (r_rdly < 4'd2) ? 4'd2 : r_rdly;
			rk <= (r_rdly < 4'd2) ? 3'd1 : r_rdly[3:1] + { 2'b00, r_rdly[0] };
			rfall <= r_rdly[0];
			cur <= (r_go[1:0] == 2'd1) ? (r_go[2] ? r_last : 24'd0) : r_adr;
			if (r_go[17] || r_go[1:0] == 2'd0) begin
				if (r_go[17]) begin
					cl <= r_cl;
					iwait <= 16'h7FFF;
					st <= S_WAIT;
				end else begin
					busy <= 1'b0;
					done_t <= ~done_t;
				end
			end else if (!inited) begin
				busy <= 1'b0;
				done_t <= ~done_t;
			end else st <= S_ACT;
		end
	end

	// open the row of cur
	S_ACT: begin
		cmd <= C_ACT;
		sba <= cur[23:22];
		sa <= cur[21:9];
		ras_cnt <= 4'd0;
		begin tcnt <= T_RCD - 4'd1; tz <= (T_RCD - 4'd1) == 4'd0; end
		st <= S_COL;
	end

	// a column: READ (pushed into the pipeline), WRITE, or READ then WRITE (S_RW)
	S_COL: begin
		if (!tz) begin tcnt <= tcnt - 4'd1; tz <= tcnt == 4'd1; end
		else begin
			sba <= cur[23:22];
			sa <= { 4'b0000, cur[8:0] };      // A10 low: no auto precharge
			if (do_rd) begin
				cmd <= C_READ;
				dqm <= 2'b00;
				dq_oe_n <= 16'hFFFF;
				push <= 1'b1;
				push_exp <= pat(rpat, cur, lfsr);
				push_adr <= cur;
				if (do_wr) begin
					// the WRITE after the data were taken (rk) and the SDRAM let the bus go (CL + 3)
					rw_n <= ({ 1'b0, rk } > { 2'b00, cl } + 4'd2) ? { 1'b0, rk } : { 2'b00, cl } + 4'd2;
					st <= S_RW;
				end else begin
					// read only: a column a clock
					if (col_last || el_last || !el) begin
						begin tcnt <= { 1'b0, rk } + 4'd2; tz <= ({ 1'b0, rk } + 4'd2) == 4'd0; end
						st <= S_DRAIN;
					end else begin
						cur <= nxt;
						lfsr <= lfsr_step(lfsr);
					end
				end
			end else begin
				cmd <= C_WRITE;
				dqm <= el ? 2'b00 : ~lanes;
				dq_out <= el ? pat(wpat, cur, lfsr) : r_dat;
				dq_oe_n <= 16'h0000;
				wr_cnt <= 4'd0;
				if (col_last || el_last || !el) begin
					begin tcnt <= 4'd0; tz <= (4'd0) == 4'd0; end
					st <= S_DRAIN;
				end else begin
					cur <= nxt;
					lfsr <= lfsr_step(lfsr);
				end
			end
		end
	end

	// a read + write column: the WRITE max(CL + 3, RDLY + 1) clocks after the READ
	S_RW: begin
		if (rw_n != 4'd0) begin
			rw_n <= rw_n - 4'd1;
			dqm <= 2'b00;
		end else begin
			cmd <= C_WRITE;
			sa <= { 4'b0000, cur[8:0] };
			dq_out <= pat(wpat, cur, lfsr);
			dq_oe_n <= 16'h0000;
			wr_cnt <= 4'd0;
			if (col_last || el_last) begin
				begin tcnt <= 4'd0; tz <= (4'd0) == 4'd0; end
				st <= S_DRAIN;
			end else begin
				cur <= nxt;
				lfsr <= lfsr_step(lfsr);
				begin tcnt <= 4'd0; tz <= (4'd0) == 4'd0; end
				st <= S_COL;
			end
		end
	end

	// the last column of the row: the reads come back, tWR after a write and tRAS pass, then PRECHARGE
	S_DRAIN: begin
		if (!tz) begin tcnt <= tcnt - 4'd1; tz <= tcnt == 4'd1; end
		else if (wr_cnt >= T_WR && ras_cnt >= T_RAS) begin
			cmd <= C_PRE;
			sa[10] <= 1'b0;
			sba <= cur[23:22];
			dq_oe_n <= 16'hFFFF;
			dqm <= 2'b11;
			begin tcnt <= T_RP - 4'd1; tz <= (T_RP - 4'd1) == 4'd0; end
			op_end <= !el || el_last;
			st <= S_PRE;
		end
	end

	S_PRE: begin
		if (!tz) begin tcnt <= tcnt - 4'd1; tz <= tcnt == 4'd1; end
		else if (ref_owed != 4'd0) begin
			cmd <= C_REF;
			ref_pay <= 1'b1;
			begin tcnt <= T_RFC; tz <= (T_RFC) == 4'd0; end
			st <= S_REF;
		end else if (op_end) begin
			busy <= 1'b0;
			done_t <= ~done_t;
			st <= S_IDLE;
		end else begin
			cur <= nxt;
			lfsr <= lfsr_step(lfsr);
			st <= S_ACT;
		end
	end

	// a refresh: then on with the operation (S_PRE) or back to idle
	S_REF: begin
		if (!tz) begin tcnt <= tcnt - 4'd1; tz <= tcnt == 4'd1; end
		else st <= busy ? S_PRE : S_IDLE;
	end

	default: st <= S_IDLE;
	endcase
end

// the data of a read: MD into the IDDR2 of each pin at both edges of clk (aligned to the rise: q_r taken at a rise,
// q_f at the fall before it), the one of RDLY kept a clock
wire [15:0] q_r, q_f;
`ifdef SIM
reg [15:0] sq_r = 16'd0, sq_f = 16'd0, sf = 16'd0;
always @(negedge clk) sf <= DQ_I;
always @(posedge clk) begin sq_r <= DQ_I; sq_f <= sf; end
assign q_r = sq_r;
assign q_f = sq_f;
`else
genvar gi;
generate for (gi = 0; gi < 16; gi = gi + 1) begin : g_dq
	IDDR2 #(.DDR_ALIGNMENT("C0"), .INIT_Q0(1'b0), .INIT_Q1(1'b0), .SRTYPE("SYNC")) iddr (
		.Q0(q_r[gi]), .Q1(q_f[gi]), .C0(clk), .C1(~clk), .CE(1'b1), .D(DQ_I[gi]), .R(1'b0), .S(1'b0));
end endgenerate
`endif
reg [15:0] dq_in_d = 16'd0;
always @(posedge clk) dq_in_d <= rfall ? q_f : q_r;

`ifdef SIM
reg [15:0] sq_o = 16'd0, sq_t = 16'hFFFF;
always @(posedge clk) begin sq_o <= dq_out; sq_t <= dq_oe_n; end
assign DQ_O = sq_o;
assign DQ_OE_N = sq_t;
`else
generate for (gi = 0; gi < 16; gi = gi + 1) begin : g_dqo
	ODDR2 #(.DDR_ALIGNMENT("C0"), .INIT(1'b0), .SRTYPE("ASYNC")) oddr_d (
		.Q(DQ_O[gi]), .C0(clk), .C1(~clk), .CE(1'b1), .D0(dq_out[gi]), .D1(dq_out[gi]), .R(1'b0), .S(1'b0));
	ODDR2 #(.DDR_ALIGNMENT("C0"), .INIT(1'b1), .SRTYPE("ASYNC")) oddr_t (
		.Q(DQ_OE_N[gi]), .C0(clk), .C1(~clk), .CE(1'b1), .D0(dq_oe_n[gi]), .D1(dq_oe_n[gi]), .R(1'b0), .S(1'b0));
end endgenerate
`endif

endmodule
