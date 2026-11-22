`timescale 1ns / 1ps
`default_nettype none
/*
 bt_sram.v

 The SRAM tester of the board test (docs/PLAN.md Ф2), on clk (clk_cpu, 75 MHz). The SRAM: two 2M x 8 chips, 10 ns,
 asynchronous, one 2M x 16 (MD[7:0] / MWR_N[0] / MRD_N[0] the low byte chip, MD[15:8] / [1] the high one). Nothing
 else uses the SRAM in this core: the tests destroy its content.

 The access cycle is the one of menu_sram (karabas-go-core-menu): MA, /WE, /OE and MD from IOB flip-flops, MD into
 an IOB flip-flop (TS_SRAM_OUT / TS_SRAM_IN of the ucf).
   read:  the address out RDCLK clocks (2 = 26.7 ns: 8 ns out, 10 ns of the SRAM, 4 ns in), MD taken at its end
   write: the address and the data out with /OE off, /WE of the lanes low a clock, /WE high a clock, the address
          and the data held a clock after /WE
 Two users of the cycle, both started by the CPU (GO), BUSY until done:
   - a direct access of one halfword (ADR, DAT, the lanes of a write): the line tests of the program;
   - an element of a march test over halfwords 0..LAST, up or down: at each address an optional read compared with
     a pattern, then an optional write of a pattern. The errors of the reads are counted (ERRORS), their wrong bits
     ORed (MASK), the first one kept (FIRST_ADR / FIRST_DAT).
 Patterns: 0 BG, 1 ~BG, 2 a 32 bit LFSR from SEED stepped at each address (its low half; up only), 3 the address
 (a[15:0] ^ a[20:16]).

 Registers (MMIO 0x80000900.., a = word in the window of bt_soc):
   00 W GO: bits 1:0 the operation (1 element, 2 direct read, 3 direct write); element: bit 2 down, bit 3 read,
        bit 4 write, bits 6:5 the read pattern, bits 8:7 the write pattern; direct write: bits 10:9 the lanes
        (bit 9 the low byte); bit 16 clears ERRORS / MASK / FIRST first (alone: only that)
   04 W ADR: the halfword of a direct access         08 W DAT: the data of a direct write
   0c W BG: the background of patterns 0 / 1          10 W SEED: the LFSR start of an element (not 0)
   14 W LAST: the last halfword of an element (2M - 1 at reset)
   18 W RDCLK: the clocks of a read (1..4, 2 at reset: 1 is under the timing of the ucf, for the margin check)
   00 R STATUS: bit 0 BUSY                            04 R CUR: the halfword of the element now
   08 R DAT: the data of the last direct read         0c R ERRORS (stops at 2^32 - 1)
   10 R MASK: the bits wrong in any read              14 R FIRST_ADR: bit 31 valid, 20:0 the halfword
   18 R FIRST_DAT: { the data read, the data expected }
*/
module bt_sram (
	input wire clk,
	// the bus of bt_soc (its window)
	input wire we,
	input wire [2:0] a,
	input wire [31:0] wd,
	output reg [31:0] rd = 32'd0,          // a clock after the address

	// pins (the top makes the tristate of MD)
	output wire [20:0] MA,
	input wire [15:0] MD_I,
	output wire [15:0] MD_O,
	output wire [15:0] MD_OE_N,
	output wire [1:0] MWR_N,
	output wire [1:0] MRD_N
);

(* IOB = "TRUE" *) reg [20:0] ma_r = 21'd0;
(* IOB = "TRUE" *) reg [1:0] mwr_r = 2'b11;
(* IOB = "TRUE" *) reg [1:0] mrd_r = 2'b11;
(* IOB = "TRUE" *) reg [15:0] md_out = 16'd0;
(* IOB = "TRUE", equivalent_register_removal = "no" *) reg [15:0] md_oe_n = 16'hFFFF;
(* IOB = "TRUE" *) reg [15:0] md_in = 16'd0;

assign MA = ma_r;
assign MWR_N = mwr_r;
assign MRD_N = mrd_r;
assign MD_O = md_out;
assign MD_OE_N = md_oe_n;

// ---------------- registers ----------------
reg [20:0] r_adr = 21'd0;
reg [15:0] r_dat = 16'd0;
reg [15:0] r_bg = 16'd0;
reg [31:0] r_seed = 32'h1;
reg [20:0] r_last = 21'h1FFFFF;
reg [2:0] r_rdclk = 3'd2;

// the operation in progress
reg busy = 1'b0;
reg el = 1'b0;                     // an element (else a direct access)
reg dn = 1'b0, do_rd = 1'b0, do_wr = 1'b0;
reg [1:0] rpat = 2'd0, wpat = 2'd0;
reg [1:0] lanes = 2'b11;
reg [20:0] cur = 21'd0;
reg [31:0] lfsr = 32'h1;
reg [15:0] exp = 16'd0;            // the data the read of cur must give
reg [15:0] dread = 16'd0;

reg [31:0] errors = 32'd0;
reg [15:0] mask = 16'd0;
reg first_v = 1'b0;
reg [20:0] first_a = 21'd0;
reg [31:0] first_d = 32'd0;

// the LFSR (Galois, x^32 + x^22 + x^2 + x + 1), one step
function [31:0] lfsr_step(input [31:0] s);
	lfsr_step = s[0] ? ((s >> 1) ^ 32'h80200003) : (s >> 1);
endfunction

function [15:0] pat(input [1:0] p, input [20:0] ad, input [31:0] l);
	case (p)
		2'd0: pat = r_bg;
		2'd1: pat = ~r_bg;
		2'd2: pat = l[15:0];
		default: pat = ad[15:0] ^ { 11'd0, ad[20:16] };
	endcase
endfunction

localparam [2:0] S_IDLE = 3'd0, S_RA = 3'd1, S_RC = 3'd2, S_W1 = 3'd3, S_W2 = 3'd4, S_W3 = 3'd5, S_ST = 3'd6;
reg [2:0] st = S_IDLE;
reg [2:0] rcnt = 3'd0;

wire el_end = dn ? (cur == 21'd0) : (cur == r_last);
wire [20:0] nxt = dn ? cur - 21'd1 : cur + 21'd1;
wire [31:0] lfsr_n = lfsr_step(lfsr);

// the first clock of an address of the element (cur, lfsr set): out the address for a read, or the data of a write
task start_adr(input [20:0] ad, input [31:0] l);
	begin
		ma_r <= ad;
		exp <= pat(rpat, ad, l);
		if (do_rd) begin
			md_oe_n <= 16'hFFFF;
			mrd_r <= 2'b00;
			rcnt <= r_rdclk - 3'd1;
			st <= S_RA;
		end else begin
			md_out <= pat(wpat, ad, l);
			md_oe_n <= 16'h0000;
			mrd_r <= 2'b11;
			st <= S_W1;
		end
	end
endtask

always @(posedge clk) begin
	md_in <= MD_I;

	// registers
	if (we) begin
		case (a)
			3'd1: r_adr <= wd[20:0];
			3'd2: r_dat <= wd[15:0];
			3'd3: r_bg <= wd[15:0];
			3'd4: r_seed <= wd;
			3'd5: r_last <= wd[20:0];
			3'd6: r_rdclk <= (wd[2:0] == 3'd0) ? 3'd1 : (wd[2:0] > 3'd4) ? 3'd4 : wd[2:0];
			default: ;
		endcase
	end
	case (a)
		3'd0: rd <= { 31'd0, busy };
		3'd1: rd <= { 11'd0, cur };
		3'd2: rd <= { 16'd0, dread };
		3'd3: rd <= errors;
		3'd4: rd <= { 16'd0, mask };
		3'd5: rd <= { first_v, 10'd0, first_a };
		default: rd <= first_d;
	endcase

	case (st)
	S_IDLE: begin
		mrd_r <= 2'b11;
		md_oe_n <= 16'hFFFF;
		if (we && a == 3'd0) begin
			if (wd[16]) begin
				errors <= 32'd0;
				mask <= 16'd0;
				first_v <= 1'b0;
			end
			el <= wd[1:0] == 2'd1;
			dn <= wd[2];
			rpat <= wd[6:5];
			wpat <= wd[8:7];
			lanes <= wd[10:9];
			case (wd[1:0])
			2'd1: begin
				// an element: from 0 up or from LAST down
				busy <= 1'b1;
				do_rd <= wd[3];
				do_wr <= wd[4];
				cur <= wd[2] ? r_last : 21'd0;
				lfsr <= r_seed;
				st <= S_ST;
			end
			2'd2: begin
				busy <= 1'b1;
				do_rd <= 1'b1;
				do_wr <= 1'b0;
				ma_r <= r_adr;
				mrd_r <= 2'b00;
				rcnt <= r_rdclk - 3'd1;
				st <= S_RA;
			end
			2'd3: begin
				busy <= 1'b1;
				do_rd <= 1'b0;
				do_wr <= 1'b1;
				ma_r <= r_adr;
				md_out <= r_dat;
				md_oe_n <= 16'h0000;
				st <= S_W1;
			end
			default: ;
			endcase
		end
	end

	// the address out: RDCLK clocks, md_in takes MD at the end of the last
	S_RA: begin
		if (rcnt == 3'd0) st <= S_RC;
		else rcnt <= rcnt - 3'd1;
	end

	// md_in holds the read: compare it (an element) or keep it (a direct read); then the write of the same address
	S_RC: begin
		if (!el) begin
			dread <= md_in;
			mrd_r <= 2'b11;
			busy <= 1'b0;
			st <= S_IDLE;
		end else begin
			if (md_in != exp) begin
				if (errors != 32'hFFFFFFFF) errors <= errors + 32'd1;
				mask <= mask | (md_in ^ exp);
				if (!first_v) begin
					first_v <= 1'b1;
					first_a <= cur;
					first_d <= { md_in, exp };
				end
			end
			if (do_wr) begin
				md_out <= pat(wpat, cur, lfsr);
				md_oe_n <= 16'h0000;
				mrd_r <= 2'b11;
				st <= S_W1;
			end else if (el_end) begin
				mrd_r <= 2'b11;
				busy <= 1'b0;
				st <= S_IDLE;
			end else begin
				cur <= nxt;
				lfsr <= lfsr_n;
				start_adr(nxt, lfsr_n);
			end
		end
	end

	S_W1: begin
		mwr_r <= el ? 2'b00 : ~lanes;
		st <= S_W2;
	end
	S_W2: begin
		mwr_r <= 2'b11;
		st <= S_W3;
	end

	// the address and the data held a clock after /WE; then the next address of an element
	S_W3: begin
		if (!el || el_end) begin
			md_oe_n <= 16'hFFFF;
			busy <= 1'b0;
			st <= S_IDLE;
		end else begin
			cur <= nxt;
			lfsr <= lfsr_n;
			start_adr(nxt, lfsr_n);
		end
	end

	// the first address of an element (cur, lfsr, do_rd, do_wr set by now)
	S_ST: start_adr(cur, lfsr);
	default: st <= S_IDLE;
	endcase
end

endmodule
