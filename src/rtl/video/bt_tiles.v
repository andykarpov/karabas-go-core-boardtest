`timescale 1ns / 1ps
`default_nettype none
/*
 bt_tiles.v

 The screen of the board test (docs/PLAN.md, "Видео: слой знакомест"): 800 x 600 at 60 Hz (the
 timing of menu_video of the menu, clk_pix 40.91 MHz), a grid of 80 x 30 cells of 10 x 20 pixels,
 all in block RAM (no frame buffer: the SRAM and the SDRAM stay free for the tests).

 A cell (the map, written by the CPU) is { bg 4, fg 4, tile 10 }: tile bit 9 = 0 a tile of the 1-bit
 atlas (the font, the big digits, the logo), 1 a tile of the 4-bit atlas (the anti-aliased shapes),
 index tile[8:0]. A tile is 10 x 20 pixels, pixel n = row * 10 + column at bit / nibble tile * 200 + n
 of its atlas (bytes, the low bit / nibble first; written by the loader, sw/tools/atlas.py).
 A pixel of coverage a (0..15, a 1-bit pixel is 0 or 15) is, per channel and for the alpha too,
 (bg * (16 - w) + fg * w) / 16 with w = a + a / 8 (0..16) of the two palette entries (16 x { alpha 4,
 RGB 24 }); bg 0 with alpha 0 is transparent: menu_compose blends the result over the gradient.

 The mouse cursor: an arrow of 12 x 19 (black and white) at cursor[10:0], cursor[25:16] (its tip), shown
 while cursor[31], over everything.

 Output: hs / vs / de with the polarity of menu_video (high active), rgba 7 clocks after the counters;
 show_o (the SHOW register): 0 = everything transparent (menu_compose shows black then).
*/
module bt_tiles (
	input wire clk_cpu,
	input wire cell_we,
	input wire [11:0] cell_a,
	input wire [17:0] cell_wd,
	output wire [17:0] cell_rd,
	input wire pal_we,
	input wire [3:0] pal_wa,
	input wire [27:0] pal_wd,
	input wire atl_we,
	input wire atl_4b,
	input wire [15:0] atl_a,
	input wire [7:0] atl_d,
	input wire [31:0] cursor,              // clk_cpu, written seldom
	input wire show,                       // clk_cpu
	output wire frame_t,                   // clk_cpu

	input wire clk_pix,
	output reg [27:0] rgba = 28'd0,
	output reg show_o = 1'b0,
	output reg hs = 1'b0,
	output reg vs = 1'b0,
	output reg de = 1'b0
);

// 800 x 600 at 60 Hz, as menu_video: the line counter steps at h 839
localparam [10:0] H_VIS = 11'd800, H_SYNC_ON = 11'd839, H_SYNC_OFF = 11'd967, H_END = 11'd1055;
localparam [9:0] V_VIS = 10'd600, V_SYNC_ON = 10'd600, V_SYNC_OFF = 10'd604, V_END = 10'd627;
localparam D = 7;                          // the stages from the counters to rgba

// ------------------------------------------------------------------ memories

// the map: port A the CPU, port B the scan
wire [17:0] map_q;
reg [11:0] map_ra = 12'd0;
kg_tdpram #(.DW(18), .AW(12), .WRITE_FIRST(0)) map (
	.clka(clk_cpu), .ena(1'b1), .wea(cell_we), .addra(cell_a), .dina(cell_wd), .douta(cell_rd),
	.clkb(clk_pix), .enb(1'b1), .web(1'b0), .addrb(map_ra), .dinb(18'd0), .doutb(map_q)
);

// the atlases (the loader writes them before the CPU starts)
wire [16:0] pos;                           // bit / nibble of the pixel in its atlas
wire [7:0] a1_q, a4_q;
kg_sdpram #(.DW(8), .AW(13)) atlas1 (      // 8 KB: 327 tiles of 1 bit
	.clka(clk_cpu), .wea(atl_we & ~atl_4b), .addra(atl_a[12:0]), .dina(atl_d),
	.clkb(clk_pix), .enb(1'b1), .addrb(pos[15:3]), .doutb(a1_q)
);
kg_sdpram #(.DW(8), .AW(12)) atlas4 (      // 4 KB: 40 tiles of 4 bits
	.clka(clk_cpu), .wea(atl_we & atl_4b), .addra(atl_a[11:0]), .dina(atl_d),
	.clkb(clk_pix), .enb(1'b1), .addrb(pos[12:1]), .doutb(a4_q)
);

// the palette, twice (fg and bg read at once)
reg [3:0] fg3 = 4'd0, bg3 = 4'd0;
wire [27:0] pfg_q, pbg_q;
kg_sdpram #(.DW(28), .AW(4)) pal_fg (
	.clka(clk_cpu), .wea(pal_we), .addra(pal_wa), .dina(pal_wd),
	.clkb(clk_pix), .enb(1'b1), .addrb(fg3), .doutb(pfg_q)
);
kg_sdpram #(.DW(28), .AW(4)) pal_bg (
	.clka(clk_cpu), .wea(pal_we), .addra(pal_wa), .dina(pal_wd),
	.clkb(clk_pix), .enb(1'b1), .addrb(bg3), .doutb(pbg_q)
);

// ------------------------------------------------------------------ the CPU registers into clk_pix

wire show_p;
kg_sync #(.W(1)) sync_show (.clk(clk_pix), .d(show), .q(show_p));
// the cursor changes seldom; a frame with a half-written position is not seen
reg [31:0] cur_s1 = 32'd0, cur = 32'd0;
always @(posedge clk_pix) begin
	cur_s1 <= cursor;
	cur <= cur_s1;
end

// ------------------------------------------------------------------ counters

reg [10:0] h = 11'd0;
reg [9:0] v = 10'd0;
reg [3:0] px = 4'd0;                       // the pixel in the cell
reg [6:0] col = 7'd0;
reg [4:0] py = 5'd0;
reg [11:0] row_base = 12'd0;               // row * 80
reg frame_p = 1'b0;
wire [9:0] v_next = (v == V_END) ? 10'd0 : v + 10'd1;
always @(posedge clk_pix) begin
	h <= (h == H_END) ? 11'd0 : h + 11'd1;
	if (h == H_END) begin
		px <= 4'd0;
		col <= 7'd0;
	end else if (px == 4'd9) begin
		px <= 4'd0;
		col <= col + 7'd1;
	end else
		px <= px + 4'd1;
	if (h == H_SYNC_ON) begin
		v <= v_next;
		if (v_next == 10'd0) begin
			py <= 5'd0;
			row_base <= 12'd0;
		end else if (py == 5'd19) begin
			py <= 5'd0;
			row_base <= row_base + 12'd80;
		end else
			py <= py + 5'd1;
	end
	if (h == 11'd0 && v == V_SYNC_ON + 10'd1) frame_p <= ~frame_p;
end
kg_sync #(.W(1)) sync_frame (.clk(clk_cpu), .d(frame_p), .q(frame_t));

// ------------------------------------------------------------------ the pixel pipeline

wire vis0 = h < H_VIS && v < V_VIS;
reg [D-1:0] vis = {D{1'b0}}, hsd = {D{1'b0}}, vsd = {D{1'b0}};

// 1: the map address, the pixel in the tile
reg [7:0] pp1 = 8'd0, pp2 = 8'd0, pp3 = 8'd0;
// 3: the tile, its colours
reg [16:0] base3 = 17'd0;
reg is4_3 = 1'b0, is4_4 = 1'b0;
reg [2:0] sel4 = 3'd0;
// 5: the coverage, the colours
reg [4:0] w5 = 5'd0;
reg [27:0] fgc5 = 28'd0, bgc5 = 28'd0;
// 6: the products
reg [11:0] pr6 = 12'd0, pg6 = 12'd0, pb6 = 12'd0;
reg [7:0] pa6 = 8'd0;
wire [8:0] idx = map_q[8:0];
assign pos = base3 + { 9'd0, pp3 };

wire [3:0] cov4 = sel4[0] ? a4_q[7:4] : a4_q[3:0];
wire cov1 = a1_q[sel4];
wire [3:0] a4 = is4_4 ? cov4 : (cov1 ? 4'd15 : 4'd0);
wire [4:0] nw = 5'd16 - w5;

always @(posedge clk_pix) begin
	vis <= { vis[D-2:0], vis0 };
	hsd <= { hsd[D-2:0], h > H_SYNC_ON && h <= H_SYNC_OFF };
	vsd <= { vsd[D-2:0], v > V_SYNC_ON && v <= V_SYNC_OFF };
	// 1
	map_ra <= row_base + { 5'd0, col };
	pp1 <= { py, 3'b000 } + { 2'b00, py, 1'b0 } + { 4'd0, px };
	// 2: map_q
	pp2 <= pp1;
	// 3
	base3 <= { idx, 7'd0 } + { 1'b0, idx, 6'd0 } + { 4'd0, idx, 3'd0 };   // idx * 200
	is4_3 <= map_q[9];
	fg3 <= map_q[13:10];
	bg3 <= map_q[17:14];
	pp3 <= pp2;
	// 4: the atlases read at pos, the palette at fg3 / bg3
	is4_4 <= is4_3;
	sel4 <= is4_3 ? { 2'b00, pos[0] } : pos[2:0];
	// 5
	w5 <= { 1'b0, a4 } + { 4'd0, a4[3] };
	fgc5 <= pfg_q;
	bgc5 <= pbg_q;
	// 6
	pa6 <= bgc5[27:24] * nw + fgc5[27:24] * w5;
	pr6 <= bgc5[23:16] * nw + fgc5[23:16] * w5;
	pg6 <= bgc5[15:8] * nw + fgc5[15:8] * w5;
	pb6 <= bgc5[7:0] * nw + fgc5[7:0] * w5;
end

// ------------------------------------------------------------------ the cursor

function [23:0] cur_row(input [4:0] y);   // 2 bits a pixel from the left: 0 none, 1 black, 2 white
	case (y)
		5'd0: cur_row = 24'h000001;
		5'd1: cur_row = 24'h000005;
		5'd2: cur_row = 24'h000019;
		5'd3: cur_row = 24'h000069;
		5'd4: cur_row = 24'h0001A9;
		5'd5: cur_row = 24'h0006A9;
		5'd6: cur_row = 24'h001AA9;
		5'd7: cur_row = 24'h006AA9;
		5'd8: cur_row = 24'h01AAA9;
		5'd9: cur_row = 24'h06AAA9;
		5'd10: cur_row = 24'h1AAAA9;
		5'd11: cur_row = 24'h556AA9;
		5'd12: cur_row = 24'h0069A9;
		5'd13: cur_row = 24'h01A469;
		5'd14: cur_row = 24'h01A419;
		5'd15: cur_row = 24'h069005;
		5'd16: cur_row = 24'h069001;
		5'd17: cur_row = 24'h1A4000;
		5'd18: cur_row = 24'h054000;
		default: cur_row = 24'h000000;
	endcase
endfunction

wire [11:0] cdx0 = { 1'b0, h } - { 1'b0, cur[10:0] };
wire [10:0] cdy0 = { 1'b0, v } - { 1'b0, cur[25:16] };
reg in1 = 1'b0;
reg [3:0] cdx1 = 4'd0, cdx2 = 4'd0;
reg [4:0] cdy1 = 5'd0;
reg [23:0] crow2 = 24'd0;
reg [1:0] cpx [3:D-1];                     // the cursor pixel from stage 3 on
integer k;
always @(posedge clk_pix) begin
	in1 <= cur[31] && cdx0 < 12'd12 && cdy0 < 11'd19;
	cdx1 <= cdx0[3:0];
	cdy1 <= cdy0[4:0];
	crow2 <= in1 ? cur_row(cdy1) : 24'd0;
	cdx2 <= cdx1;
	cpx[3] <= crow2 >> { cdx2, 1'b0 };
	for (k = 4; k < D; k = k + 1) cpx[k] <= cpx[k - 1];
end

// ------------------------------------------------------------------ out (7)

always @(posedge clk_pix) begin
	if (cpx[D-1] == 2'd1)
		rgba <= 28'hF000000;
	else if (cpx[D-1] == 2'd2)
		rgba <= 28'hFFFFFFF;
	else if (vis[D-2] && show_p)
		rgba <= { pa6[7:4], pr6[11:4], pg6[11:4], pb6[11:4] };
	else
		rgba <= 28'd0;
	show_o <= show_p;
	de <= vis[D-2];
	hs <= hsd[D-2];
	vs <= vsd[D-2];
end

endmodule
