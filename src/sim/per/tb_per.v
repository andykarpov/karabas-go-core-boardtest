`timescale 1ns / 1ps
`default_nettype none
// bt_per (rtl/io/bt_per.v) against small models: an SPI slave (mode 0) that answers each byte with the byte before
// it + 1, the uart looped back (UART_TX -> UART_RX, with CTS), an IDE device of 8 task file registers (sector count
// and LBA read back, status 0x58, the data register gives 0x1234 + n). iverilog, a second.
module tb_per;
reg clk = 1'b0;
always #6.667 clk = ~clk;

reg we = 1'b0, rd_go = 1'b0;
reg [3:0] a = 4'd0;
reg [31:0] wd = 32'd0;
wire [31:0] rd;
wire own_sd, own_ft, sck, mosi, sd_cs_n, ft_cs_n, esp32_cs_n, uart_tx, uart_cts;
wire [2:0] ide_a;
wire [1:0] ide_cs_n;
wire ide_rd_n, ide_wr_n, ide_rst_n, ide_oe;
wire [15:0] ide_do;
reg [15:0] ide_di = 16'hffff;
reg miso = 1'b1;
reg mcu_ft_on = 1'b0;

bt_per #(.CLKS_PER_BIT(16)) dut (
	.clk(clk), .we(we), .a(a), .wd(wd), .rd(rd), .rd_go(rd_go),
	.own_sd(own_sd), .own_ft(own_ft), .mcu_ft_on(mcu_ft_on), .spi_sck(sck), .spi_mosi(mosi), .sd_cs_n(sd_cs_n),
	.ft_cs_n(ft_cs_n), .esp32_cs_n(esp32_cs_n), .sd_miso(miso), .ft_miso(miso), .sd_det_n(1'b0), .ft_int_n(1'b1),
	.uart_rx(uart_tx), .uart_tx(uart_tx), .uart_cts(uart_cts),
	.ide_a(ide_a), .ide_cs_n(ide_cs_n), .ide_rd_n(ide_rd_n), .ide_wr_n(ide_wr_n), .ide_rst_n(ide_rst_n),
	.ide_d_i(ide_di), .ide_d_o(ide_do), .ide_d_oe(ide_oe), .fdd_out(), .fdd_in(4'd0)
);

// ---- the SPI slave: shifts MOSI in on the rising edge, the answer out on the falling one
reg [7:0] s_in = 8'd0, s_out = 8'h5a, s_prev = 8'h00;
reg [2:0] s_n = 3'd0;
wire s_cs = ~sd_cs_n | ~ft_cs_n | ~esp32_cs_n;
always @(negedge s_cs) begin s_n = 0; miso = s_out[7]; end
always @(posedge sck) if (s_cs) begin
	s_in = { s_in[6:0], mosi };
	s_n = s_n + 3'd1;
	if (s_n == 3'd0) begin s_prev = s_in; s_out = s_in + 8'd1; end
end
always @(negedge sck) if (s_cs) miso = s_n == 3'd0 ? s_out[7] : s_out[7 - s_n];

// ---- the IDE device
reg [7:0] tf [0:7];
reg [15:0] dn = 16'd0;
integer k;
initial for (k = 0; k < 8; k = k + 1) tf[k] = 8'h00;
always @(negedge ide_wr_n) #1 if (ide_cs_n == 2'b10) tf[ide_a] = ide_do[7:0];
always @(negedge ide_rd_n) begin
	#100;
	if (ide_cs_n == 2'b10) ide_di = ide_a == 3'd0 ? 16'h1234 + dn : ide_a == 3'd7 ? 16'h0058 : { 8'h00, tf[ide_a] };
	else ide_di = 16'h0050;
end
always @(posedge ide_rd_n) begin #20; if (ide_cs_n == 2'b10 && ide_a == 3'd0) dn = dn + 16'd1; ide_di = 16'hffff; end

// ---- the bus
task wr(input [3:0] ad, input [31:0] v);
	begin @(posedge clk); a <= ad; wd <= v; we <= 1'b1; @(posedge clk); we <= 1'b0; #1; end
endtask
reg [31:0] q;
task rdr(input [3:0] ad);
	begin @(posedge clk); a <= ad; rd_go <= 1'b1; @(posedge clk); rd_go <= 1'b0; @(posedge clk); q = rd; end
endtask
task spi_wait; begin rdr(4'd0); while (q[0]) rdr(4'd0); end endtask

integer errors = 0, i;
initial begin
	repeat (4) @(posedge clk);
	// SPI: the SD2 slot, half period 2
	wr(4'd0, 32'h0000020d);   // dev 1, cs on, own sd
	if (sd_cs_n !== 1'b0 || !own_sd) begin $display("FAIL spi cs"); errors = errors + 1; end
	wr(4'd1, 32'h000000a5); spi_wait;
	if (s_prev !== 8'ha5) begin $display("FAIL spi mosi %h", s_prev); errors = errors + 1; end
	rdr(4'd1);
	if (q[7:0] !== 8'h5a) begin $display("FAIL spi miso %h", q[7:0]); errors = errors + 1; end
	// read-and-next: answers a6, then ff + 1 = 00
	rdr(4'd2); spi_wait; rdr(4'd2); spi_wait; rdr(4'd1);
	if (q[7:0] !== 8'h00) begin $display("FAIL spi next %h", q[7:0]); errors = errors + 1; end
	// the FT812 bus is not taken while the MCU has it
	mcu_ft_on = 1'b1;
	wr(4'd0, 32'h00000216);
	if (ft_cs_n !== 1'b1) begin $display("FAIL ft cs while the MCU has it"); errors = errors + 1; end
	mcu_ft_on = 1'b0;
	@(posedge clk);
	if (ft_cs_n !== 1'b0) begin $display("FAIL ft cs"); errors = errors + 1; end
	wr(4'd0, 32'h0000001f);   // the ESP32, half 1 (0 taken as 1)
	if (esp32_cs_n !== 1'b0 || ft_cs_n !== 1'b1) begin $display("FAIL esp32 cs"); errors = errors + 1; end
	wr(4'd1, 32'h0000003c); spi_wait;
	if (s_prev !== 8'h3c) begin $display("FAIL spi fast %h", s_prev); errors = errors + 1; end
	wr(4'd0, 32'h00000000);

	// the uart: 3 bytes looped back
	wr(4'd4, 32'h41); wr(4'd4, 32'h54); wr(4'd4, 32'h0d);
	repeat (3 * 11 * 16 + 100) @(posedge clk);
	rdr(4'd5);
	if (q[11:0] !== 12'd3) begin $display("FAIL uart count %0d", q[11:0]); errors = errors + 1; end
	rdr(4'd4); if (q[8:0] !== 9'h141) begin $display("FAIL uart 0 %h", q); errors = errors + 1; end
	rdr(4'd4); if (q[8:0] !== 9'h154) begin $display("FAIL uart 1 %h", q); errors = errors + 1; end
	rdr(4'd4); if (q[8:0] !== 9'h10d) begin $display("FAIL uart 2 %h", q); errors = errors + 1; end
	rdr(4'd4); if (q[8] !== 1'b0) begin $display("FAIL uart empty %h", q); errors = errors + 1; end
	wr(4'd5, 32'h1);
	if (uart_cts !== 1'b1) begin $display("FAIL cts"); errors = errors + 1; end
	wr(4'd5, 32'h0);

	// IDE: write the sector count, read it back, the status, two data words
	wr(4'd8, 32'h00550012);   // write, CS0, register 2
	rdr(4'd8); while (q[0]) rdr(4'd8);
	wr(4'd8, 32'h00000002);
	rdr(4'd8); while (q[0]) rdr(4'd8);
	rdr(4'd9); if (q[7:0] !== 8'h55) begin $display("FAIL ide reg %h", q); errors = errors + 1; end
	wr(4'd8, 32'h00000007);
	rdr(4'd8); while (q[0]) rdr(4'd8);
	rdr(4'd9); if (q[7:0] !== 8'h58) begin $display("FAIL ide status %h", q); errors = errors + 1; end
	for (i = 0; i < 2; i = i + 1) begin
		wr(4'd8, 32'h00000000);
		rdr(4'd8); while (q[0]) rdr(4'd8);
		rdr(4'd9); if (q[15:0] !== 16'h1234 + i) begin $display("FAIL ide data %h", q); errors = errors + 1; end
	end
	wr(4'd8, 32'h0000000e);   // CS1, the alternate status
	rdr(4'd8); while (q[0]) rdr(4'd8);
	rdr(4'd9); if (q[7:0] !== 8'h50) begin $display("FAIL ide cs1 %h", q); errors = errors + 1; end
	wr(4'd10, 32'h1);
	if (ide_rst_n !== 1'b0) begin $display("FAIL ide reset"); errors = errors + 1; end

	if (errors == 0) $display("PASS"); else $display("FAIL %0d", errors);
	$finish;
end
initial begin #2000000; $display("FAIL timeout"); $finish; end
endmodule
