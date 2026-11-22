`timescale 1ns / 1ps
`default_nettype none
// tb_bt: bt_soc + bt_tiles + menu_compose + kg_l2_rpc as the top wires them (no MCU link, no HDMI); the clocks,
// the loader, the model of the MCU on the link side of kg_l2_rpc and the frame capture are in tb_bt.cpp.
module tb_bt (
	input wire clk_cpu,
	input wire clk_pix,
	input wire reset,
	input wire ld_active,
	input wire ld_end,
	input wire [31:0] ld_addr,
	input wire [7:0] ld_data,
	input wire ld_wr,
	output wire [7:0] dbg_uart,
	output wire dbg_uart_wr,
	output wire cpu_run,
	output wire [23:0] rgb,
	output wire hs,
	output wire vs,
	output wire de,
	input wire [55:0] kbd,
	input wire kbd_wr,
	input wire [7:0] ms_x,
	input wire [7:0] ms_y,
	input wire ms_wr,
	// the link side of kg_l2_rpc (the model of the MCU)
	input wire l_sel,
	input wire l_stb,
	input wire [7:0] l_cmd,
	input wire [9:0] l_n,
	input wire [7:0] l_d,
	input wire l_rq,
	input wire [9:0] l_rq_m,
	output wire [7:0] l_q,
	output wire l_irq,
	// what the CPU writes to the screen (the .cpp keeps a copy for the model of the frame)
	output wire cell_we,
	output wire [11:0] cell_a,
	output wire [17:0] cell_wd,
	output wire pal_we,
	output wire [3:0] pal_wa,
	output wire [27:0] pal_wd,
	output wire [23:0] grad_top,
	output wire [23:0] grad_bot,
	output wire [31:0] cursor
);
wire [10:0] rpc_a;
wire rpc_we;
wire [7:0] rpc_wd, rpc_rd;
kg_l2_rpc #(.SERVICES(16'h0007)) rpc (
	.clk(clk_cpu), .reset(reset),
	.sel(l_sel), .stb(l_stb), .cmd(l_cmd), .n(l_n), .d(l_d), .rq(l_rq), .rq_m(l_rq_m), .q(l_q), .irq(l_irq),
	.cpu_a(rpc_a), .cpu_we(rpc_we), .cpu_wd(rpc_wd), .cpu_rd(rpc_rd)
);

wire atl_we, atl_4b, show, frame_t;
wire [15:0] atl_a;
wire [7:0] atl_d;
wire [17:0] cell_rd;
bt_soc soc (
	.clk(clk_cpu), .reset(reset),
	.ld_kind(2'd0), .ld_active(ld_active), .ld_end(ld_end), .ld_addr(ld_addr), .ld_data(ld_data),
	.ld_wr(ld_wr), .ld_ready(),
	.kbd(kbd), .kbd_wr(kbd_wr), .ms_x(ms_x), .ms_y(ms_y), .ms_z(8'd0), .ms_b(8'd0), .ms_wr(ms_wr),
	.joy_l(16'd0), .joy_r(16'd0),
	.cell_we(cell_we), .cell_a(cell_a), .cell_wd(cell_wd), .cell_rd(cell_rd),
	.pal_we(pal_we), .pal_wa(pal_wa), .pal_wd(pal_wd),
	.atl_we(atl_we), .atl_4b(atl_4b), .atl_a(atl_a), .atl_d(atl_d),
	.cursor(cursor), .grad_top(grad_top), .grad_bot(grad_bot), .show(show), .frame_t(frame_t),
	.bio_we(), .bio_rd_go(), .bio_a(), .bio_wd(), .bio_rd(32'd0),
	.rpc_a(rpc_a), .rpc_we(rpc_we), .rpc_wd(rpc_wd), .rpc_rd(rpc_rd),
	.dbg_uart(dbg_uart), .dbg_uart_wr(dbg_uart_wr), .cpu_run(cpu_run)
);

wire [27:0] t_rgba;
wire t_on, t_hs, t_vs, t_de;
bt_tiles tiles (
	.clk_cpu(clk_cpu),
	.cell_we(cell_we), .cell_a(cell_a), .cell_wd(cell_wd), .cell_rd(cell_rd),
	.pal_we(pal_we), .pal_wa(pal_wa), .pal_wd(pal_wd),
	.atl_we(atl_we), .atl_4b(atl_4b), .atl_a(atl_a), .atl_d(atl_d),
	.cursor(cursor), .show(show), .frame_t(frame_t),
	.clk_pix(clk_pix), .rgba(t_rgba), .show_o(t_on), .hs(t_hs), .vs(t_vs), .de(t_de)
);

menu_compose compose (
	.clk(clk_pix), .rgba_i(t_rgba), .show_i(t_on), .hs_i(t_hs), .vs_i(t_vs), .de_i(t_de),
	.glow_i(24'd0), .grad_top(grad_top), .grad_bot(grad_bot),
	.rgb_o(rgb), .hs_o(hs), .vs_o(vs), .de_o(de)
);

endmodule
