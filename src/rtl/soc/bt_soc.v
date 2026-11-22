`timescale 1ns / 1ps
`default_nettype none
/*
 bt_soc.v

 The computer of the board test (docs/PLAN.md): VexRiscv (rv32im, I$ 4K, D$ 4K, wishbone; the
 netlist of karabas-go-core-menu / doom as it is) with its program and data in block RAM. Nothing of
 the UI is in the SRAM or the SDRAM: they are free for the tests. One clock (clk, 75 MHz). After
 menu_soc.v of karabas-go-core-menu.

 Memory map of the CPU (sw/mmio.h):
   0x00000000 RAM, (1 << MEM_AW) words (block RAM, the program from 0: the ROM of boardtest.kg)
   0x80000000 MMIO (uncached: the VexRiscv IO range = bit 31), word registers:
     00 W  DBG: a byte of the debug console (dbg_uart, the testbench prints it)
     08 R  FRAMES: frames begun (the start of the vertical sync)
     0c R  CYCLES: clocks since the CPU started (low 32)
     10 W  SHOW: bit 0 the tile screen on (0: black)
     18 W  GRAD_TOP: 0x00RRGGBB, the top colour of the background gradient (menu_compose)
     20 W  GRAD_BOT: 0x00RRGGBB, its bottom colour (3/4 of the height and below)
     1c R  MS: milliseconds since the CPU started       40 R  US: microseconds
     28 R  KBD0: kg_mcu2 KBD[31:0] = { key 3, key 2, key 1, modifiers } (the USB boot report)
     2c R  KBD1: { 0, key 6, key 5, key 4 }            30 R  KBDN: reports seen (a counter)
     34 R  MOUSE: bits 31..24 buttons (held now, or pressed since the last read), 23..12 dy sum,
           11..0 dx sum (signed, saturated; reading clears)
     38 R  JOY: { joy_r, joy_l }
     3c R  WHEEL: bits 11..0 the wheel sum (signed, saturated; + = away from the user; reading clears)
     44 W  CURSOR: bit 31 shown, bits 25..16 y, bits 10..0 x (the hot spot, pixels)
     400..43c W  PAL: entry (a - 400) / 4 of the 16 colours of the cells = 0xARRGGBB: alpha 0..15 in
                 bits 31..28 (0 transparent, 15 opaque)
     2000..3ffc  RPC: byte (a - 2000) / 4 of the mailbox of kg_l2_rpc (kg_mcu2 RPC_*), in the low byte
     10000..1257c CELLS: cell (a - 10000) / 4 = row * 80 + column, bits 17..0 { bg 4, fg 4, tile 10 }
                 (bt_tiles), read / write
 Fetches and data use the two ports of the RAM; an access takes two clocks (the ack a clock after
 the address, as the block RAM reads).

 Loader (kg_mcu2 LD_*, kind 0, the ROMs of boardtest.kg): a byte per write, by address: below
 0x100000 the RAM (while the CPU is held in reset), 0x100000 the 1-bit atlas, 0x110000 the 4-bit
 atlas of bt_tiles. The CPU is held in reset while anything loads and until the first load has
 ended (no program before it); ld_ready is always 1 (a write takes one clock).
*/
module bt_soc #(
	parameter MS_DIV = 17'd75000,         // clocks per millisecond
	parameter US_DIV = 7'd75,             // clocks per microsecond
	parameter MEM_AW = 13,                // RAM words: 8K = 32 KB
	parameter [31:0] RESET_VECTOR = 32'h00000000
) (
	input wire clk,
	input wire reset,                     // the PLL

	// loader (kg_mcu2)
	input wire [1:0] ld_kind,
	input wire ld_active,
	input wire ld_end,
	input wire [31:0] ld_addr,
	input wire [7:0] ld_data,
	input wire ld_wr,
	output wire ld_ready,

	// HID (kg_mcu2)
	input wire [55:0] kbd,
	input wire kbd_wr,
	input wire [7:0] ms_x,
	input wire [7:0] ms_y,
	input wire [7:0] ms_z,
	input wire [7:0] ms_b,
	input wire ms_wr,
	input wire [15:0] joy_l,
	input wire [15:0] joy_r,

	// the tile screen (bt_tiles, clk side)
	output reg cell_we = 1'b0,
	output reg [11:0] cell_a = 12'd0,
	output reg [17:0] cell_wd = 18'd0,
	input wire [17:0] cell_rd,
	output reg pal_we = 1'b0,
	output reg [3:0] pal_wa = 4'd0,
	output reg [27:0] pal_wd = 28'd0,
	output reg atl_we = 1'b0,
	output reg atl_4b = 1'b0,              // 0: the 1-bit atlas, 1: the 4-bit one
	output reg [15:0] atl_a = 16'd0,
	output reg [7:0] atl_d = 8'd0,
	output reg [31:0] cursor = 32'd0,
	output reg [23:0] grad_top = 24'h000000,  // black until the CPU writes the colours of karabas.ini
	output reg [23:0] grad_bot = 24'h000000,
	output reg show = 1'b0,
	input wire frame_t,                   // toggles at each frame (synchronised to clk)

	// RPC (kg_mcu2 RPC_*)
	output reg [10:0] rpc_a = 11'd0,
	output reg rpc_we = 1'b0,
	output reg [7:0] rpc_wd = 8'd0,
	input wire [7:0] rpc_rd,

	// debug
	output reg [7:0] dbg_uart = 8'd0,
	output reg dbg_uart_wr = 1'b0,
	output wire cpu_run
);

// ------------------------------------------------------------------ reset / loader

reg loaded = 1'b0;                    // a program came (the end of a kind 0 load)
reg cpu_reset = 1'b1;
assign cpu_run = ~cpu_reset;
assign ld_ready = 1'b1;
always @(posedge clk) begin
	if (reset) loaded <= 1'b0;
	else if (ld_end && ld_kind == 2'd0) loaded <= 1'b1;
	cpu_reset <= reset | ld_active | ~loaded;
end

wire ld_k0 = ld_wr && ld_kind == 2'd0;
wire ld_ram = ld_k0 && ld_addr[31:20] == 12'd0;
wire ld_atl = ld_k0 && ld_addr[31:20] == 12'd1;

// ------------------------------------------------------------------ CPU

wire i_cyc, i_stb, d_cyc, d_stb, d_we;
wire [29:0] i_adr, d_adr;
wire [31:0] d_dat_o;
wire [3:0] d_sel;
wire [2:0] i_cti, d_cti;
reg [31:0] d_dat_i;
reg i_ack = 1'b0, d_ack;
wire [31:0] i_dat_i;

VexRiscv cpu (
	.externalResetVector(RESET_VECTOR),
	.timerInterrupt(1'b0),
	.softwareInterrupt(1'b0),
	.externalInterruptArray(32'd0),
	.iBusWishbone_CYC(i_cyc),
	.iBusWishbone_STB(i_stb),
	.iBusWishbone_ACK(i_ack),
	.iBusWishbone_WE(),
	.iBusWishbone_ADR(i_adr),
	.iBusWishbone_DAT_MISO(i_dat_i),
	.iBusWishbone_DAT_MOSI(),
	.iBusWishbone_SEL(),
	.iBusWishbone_ERR(1'b0),
	.iBusWishbone_CTI(i_cti),
	.iBusWishbone_BTE(),
	.dBusWishbone_CYC(d_cyc),
	.dBusWishbone_STB(d_stb),
	.dBusWishbone_ACK(d_ack),
	.dBusWishbone_WE(d_we),
	.dBusWishbone_ADR(d_adr),
	.dBusWishbone_DAT_MISO(d_dat_i),
	.dBusWishbone_DAT_MOSI(d_dat_o),
	.dBusWishbone_SEL(d_sel),
	.dBusWishbone_ERR(1'b0),
	.dBusWishbone_CTI(d_cti),
	.dBusWishbone_BTE(),
	.clk(clk),
	.reset(cpu_reset)
);

wire d_req = d_cyc & d_stb;
wire i_req = i_cyc & i_stb;
wire d_io = d_adr[29];                      // 0x80000000

// ------------------------------------------------------------------ RAM: port A data (or the loader), port B fetch

reg m_ack = 1'b0;
wire [3:0] ram_we = cpu_reset ? (ld_ram ? (4'b0001 << ld_addr[1:0]) : 4'b0000)
                              : ((d_req && !d_io && d_we && !m_ack) ? d_sel : 4'b0000);
wire [MEM_AW-1:0] ram_aa = cpu_reset ? ld_addr[MEM_AW+1:2] : d_adr[MEM_AW-1:0];
wire [31:0] ram_da = cpu_reset ? {4{ld_data}} : d_dat_o;
wire [31:0] ram_qa;
kg_tdpram #(.DW(32), .AW(MEM_AW), .WE_W(4), .WRITE_FIRST(0)) ram (
	.clka(clk), .ena(1'b1), .wea(ram_we), .addra(ram_aa), .dina(ram_da), .douta(ram_qa),
	.clkb(clk), .enb(1'b1), .web(4'b0000), .addrb(i_adr[MEM_AW-1:0]), .dinb(32'd0), .doutb(i_dat_i)
);

// an access: the address is read at the first edge, the ack with the data a clock later (a burst
// holds STB and moves the address after each ack)
always @(posedge clk) begin
	i_ack <= i_req & ~i_ack & ~cpu_reset;
	m_ack <= d_req & ~d_io & ~m_ack & ~cpu_reset;
end

// ------------------------------------------------------------------ MMIO

reg [31:0] cycles = 32'd0;
reg [31:0] ms = 32'd0;
reg [16:0] ms_cnt = 17'd0;
reg [31:0] us = 32'd0;
reg [6:0] us_div = 7'd0;
reg [31:0] kbd_n = 32'd0;
reg [31:0] frames = 32'd0;
reg frame_p = 1'b0;
reg signed [11:0] ms_dx = 12'sd0, ms_dy = 12'sd0, ms_dz = 12'sd0;
reg [7:0] ms_btn = 8'd0;
reg [7:0] ms_press = 8'd0;   // the buttons pressed since the last read
reg io_ack = 1'b0;
reg [31:0] io_dat = 32'd0;
wire [11:0] io_off = { d_adr[9:0], 2'b00 };
wire io_cell = d_adr[14];                    // 0x10000..
wire io_rpc = ~d_adr[14] & d_adr[11];        // 0x2000..0x3ffc (word address bit 11)
// an RPC or cell read: the RAM registers the address, then its output: the data two clocks after
reg rd_wait = 1'b0, rd_wait2 = 1'b0, rd_cell = 1'b0;

function signed [11:0] sat_add(input signed [11:0] a, input [7:0] d);
	reg signed [12:0] s;
	begin
		s = { a[11], a } + { {5{d[7]}}, d };
		sat_add = (s > 13'sd2047) ? 12'sd2047 : (s < -13'sd2048) ? -12'sd2048 : s[11:0];
	end
endfunction

wire io_go = d_req && d_io && !io_ack && !rd_wait && !rd_wait2 && !cpu_reset;

always @(posedge clk) begin
	io_ack <= 1'b0;
	pal_we <= 1'b0;
	cell_we <= 1'b0;
	dbg_uart_wr <= 1'b0;
	rpc_we <= 1'b0;
	atl_we <= 1'b0;
	// the atlases from the loader
	if (ld_atl) begin
		atl_we <= 1'b1;
		atl_4b <= ld_addr[16];
		atl_a <= ld_addr[15:0];
		atl_d <= ld_data;
	end
	rd_wait2 <= rd_wait;
	rd_wait <= 1'b0;
	if (rd_wait2) begin
		io_ack <= 1'b1;
		io_dat <= rd_cell ? { 14'd0, cell_rd } : { 24'd0, rpc_rd };
	end
	if (cpu_reset) begin
		cycles <= 32'd0;
		ms <= 32'd0;
		ms_cnt <= 17'd0;
		us <= 32'd0;
		us_div <= 7'd0;
	end else begin
		cycles <= cycles + 32'd1;
		if (ms_cnt == MS_DIV - 17'd1) begin ms_cnt <= 17'd0; ms <= ms + 32'd1; end
		else ms_cnt <= ms_cnt + 17'd1;
		if (us_div == US_DIV - 7'd1) begin us_div <= 7'd0; us <= us + 32'd1; end
		else us_div <= us_div + 7'd1;
	end
	frame_p <= frame_t;
	if (frame_t != frame_p) frames <= frames + 32'd1;
	if (kbd_wr) kbd_n <= kbd_n + 32'd1;
	if (ms_wr) begin
		ms_dx <= sat_add(ms_dx, ms_x);
		ms_dy <= sat_add(ms_dy, ms_y);
		ms_dz <= sat_add(ms_dz, ms_z);
		ms_btn <= ms_b;
		ms_press <= ms_press | ms_b;
	end
	if (reset) begin
		show <= 1'b0;
		cursor[31] <= 1'b0;
	end

	if (io_go && (io_cell || io_rpc)) begin
		rd_cell <= io_cell;
		if (io_cell) begin
			cell_a <= d_adr[11:0];
			cell_wd <= d_dat_o[17:0];
		end else
			rpc_a <= d_adr[10:0];
		if (d_we) begin
			if (io_cell) cell_we <= 1'b1;
			else begin rpc_wd <= d_dat_o[7:0]; rpc_we <= 1'b1; end
			io_ack <= 1'b1;
		end else
			rd_wait <= 1'b1;
	end else if (io_go) begin
		io_ack <= 1'b1;
		if (d_we) begin
			if (io_off[11:6] == 6'b010000) begin   // 0x400..0x43c
				pal_we <= 1'b1;
				pal_wa <= io_off[5:2];
				pal_wd <= { d_dat_o[31:28], d_dat_o[23:0] };
			end else case (io_off[7:0])
				8'h00: begin dbg_uart <= d_dat_o[7:0]; dbg_uart_wr <= 1'b1; end
				8'h10: show <= d_dat_o[0];
				8'h18: grad_top <= d_dat_o[23:0];
				8'h20: grad_bot <= d_dat_o[23:0];
				8'h44: cursor <= d_dat_o;
				default: ;
			endcase
		end else begin
			case (io_off[7:0])
				8'h08: io_dat <= frames;
				8'h0c: io_dat <= cycles;
				8'h1c: io_dat <= ms;
				8'h28: io_dat <= kbd[31:0];
				8'h2c: io_dat <= { 8'd0, kbd[55:32] };
				8'h30: io_dat <= kbd_n;
				8'h34: begin
					io_dat <= { ms_btn | ms_press, ms_dy, ms_dx };
					ms_press <= ms_wr ? ms_b : 8'd0;
					if (ms_wr) begin
						ms_dx <= sat_add(12'sd0, ms_x);
						ms_dy <= sat_add(12'sd0, ms_y);
					end else begin
						ms_dx <= 12'sd0;
						ms_dy <= 12'sd0;
					end
				end
				8'h38: io_dat <= { joy_r, joy_l };
				8'h3c: begin
					io_dat <= { {20{ms_dz[11]}}, ms_dz };
					ms_dz <= ms_wr ? sat_add(12'sd0, ms_z) : 12'sd0;
				end
				8'h40: io_dat <= us;
				8'h44: io_dat <= cursor;
				default: io_dat <= 32'd0;
			endcase
		end
	end
end

// ------------------------------------------------------------------ CPU bus answers

always @(*) begin
	d_ack = m_ack | io_ack;
	d_dat_i = d_io ? io_dat : ram_qa;
end

endmodule
