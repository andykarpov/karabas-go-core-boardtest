`timescale 1ns / 1ps
`default_nettype none
/*
 bt_per.v

 The storage and the modules of the board test CPU (docs/PLAN.md Ф3), on clk (clk_cpu, 75 MHz): MMIO
 0x80000b00..0x80000bfc of bt_soc, word registers (a = byte offset from 0xb00):

 SPI master, mode 0 (SCK low at rest, MOSI set before the rising edge, MISO taken before the falling one):
   00 W  SPI_CTL: bits 1:0 the device (1 the SD2 slot, 2 the FT812, 3 the ESP32: it shares the bus of the FT812),
         bit 2 its chip select on, bit 3 the core has the SD2 slot (else the MCU: kg_mcu2 passes it through),
         bit 4 the core has the FT812 bus (never while the MCU has it: FT_SPI_ON), bits 15:8 the half period of
         SCK - 1 in clocks (0 is taken as 1: SCK 75 / (2 (h + 1)) MHz, 18.75 at most)
      R  bit 0 a byte in flight, bit 1 the MCU has the FT812 bus, bit 2 a card in the SD2 slot (SD_DET_N low),
         bit 3 FT_INT_N, bits 15:8 the SPI_CTL written
   04 W  SPI_DATA: a byte out (the transfer starts); R the byte of the last transfer
   08 R  SPI_NEXT: the byte of the last transfer, and the next transfer of 0xFF starts (a read of a block)
 the uart of the ESP8266 (115200 8N1):
   10 W  UART_TX: a byte to the fifo of 64; R bit 8 a byte from the ESP is there (bits 7..0, reading takes it),
         bit 9 the tx fifo is full, bit 10 bytes were lost (the rx fifo of 1024 full; reading clears), bit 11 UART_RX
   14 W  UART_CTL: bit 0 CTS held high (the ESP must stop sending), bit 1 the rx fifo emptied
      R  bits 10:0 bytes in the rx fifo
 the IDE / CF bus, PIO mode 0 (a cycle of 600 ns: address 6 clocks before the strobe, the strobe 24, 3 after it,
 then 12 of recovery):
   20 W  IDE_GO: bits 2:0 the register (DA2..0), bit 3 CS1 (the control block) else CS0, bit 4 a write, bits 31:16
         the data of a write; the cycle starts. R bit 0 a cycle runs
   24 R  IDE_DATA: the 16 bits of the last read cycle
   28 W  IDE_RST: bit 0 RESET_N held low
 the floppy port of the Go (the pins high = active: the board inverts them to the connector; all 0 at rest):
   30 W  FDD_OUT: bits 1:0 the drive selects, bit 2 the motor, bit 3 DIR, bit 4 STEP, bit 5 the side (SIDE_N pin: 1 =
         side 1); WDATA and WGATE stay 0 (nothing is written)
      R  bit 0 INDEX, 1 TR00, 2 WPRT, 3 RDATA now; bits 31:8 the INDEX pulses (counting)
   34 R  FDD_FLUX: the RDATA pulses (counting)
*/
module bt_per #(
	parameter integer CLKS_PER_BIT = 651          // 75 MHz / 115200
) (
	input wire clk,
	// the bus of bt_soc
	input wire we,
	input wire [3:0] a,                           // word address in the window
	input wire [31:0] wd,
	output reg [31:0] rd = 32'd0,                 // a clock after the address
	input wire rd_go,
	// SPI: the owners, the lines (the top muxes them with the MCU)
	output reg own_sd = 1'b0,
	output reg own_ft = 1'b0,
	input wire mcu_ft_on,
	output reg spi_sck = 1'b0,
	output reg spi_mosi = 1'b1,
	output wire sd_cs_n,
	output wire ft_cs_n,
	output wire esp32_cs_n,
	input wire sd_miso,
	input wire ft_miso,
	input wire sd_det_n,
	input wire ft_int_n,
	// the uart of the ESP8266
	input wire uart_rx,
	output reg uart_tx = 1'b1,
	output wire uart_cts,
	// IDE / CF
	output reg [2:0] ide_a = 3'd0,
	output reg [1:0] ide_cs_n = 2'b11,
	output reg ide_rd_n = 1'b1,
	output reg ide_wr_n = 1'b1,
	output reg ide_rst_n = 1'b1,
	input wire [15:0] ide_d_i,
	output reg [15:0] ide_d_o = 16'd0,
	output reg ide_d_oe = 1'b0,
	// the floppy port (Go)
	output reg [5:0] fdd_out = 6'd0,          // { side, step, dir, motor, drive[1:0] }
	input wire [3:0] fdd_in                   // { rdata, wprt, tr00, index }
);

// ------------------------------------------------------------------ the floppy port

reg [3:0] fdd_s1 = 4'd0, fdd_s2 = 4'd0, fdd_s3 = 4'd0;
reg [23:0] fdd_idx = 24'd0;
reg [31:0] fdd_flux = 32'd0;
always @(posedge clk) begin
	fdd_s1 <= fdd_in; fdd_s2 <= fdd_s1; fdd_s3 <= fdd_s2;
	if (fdd_s2[0] & ~fdd_s3[0]) fdd_idx <= fdd_idx + 24'd1;
	if (fdd_s2[3] & ~fdd_s3[3]) fdd_flux <= fdd_flux + 32'd1;
	if (we && a == 4'd12) fdd_out <= wd[5:0];
end

// ------------------------------------------------------------------ SPI

reg [1:0] spi_dev = 2'd0;
reg spi_cs = 1'b0;
reg [7:0] spi_half = 8'd3;
reg [7:0] spi_sh = 8'hff, spi_in = 8'hff, spi_last = 8'hff;
reg [3:0] spi_bit = 4'd0;                    // the edges left: 16 per byte
reg [7:0] spi_cnt = 8'd0;
reg spi_busy = 1'b0;
reg [1:0] det_s = 2'b11, int_s = 2'b11;
reg miso_r = 1'b1;
wire own_ft_now = own_ft & ~mcu_ft_on;
assign sd_cs_n = ~(spi_cs && spi_dev == 2'd1);
assign ft_cs_n = ~(spi_cs && spi_dev == 2'd2 && own_ft_now);
assign esp32_cs_n = ~(spi_cs && spi_dev == 2'd3 && own_ft_now);
wire miso = spi_dev == 2'd1 ? sd_miso : ft_miso;

wire spi_start_w = we && a == 4'd1 && !spi_busy;
wire spi_start_n = rd_go && a == 4'd2 && !spi_busy;

always @(posedge clk) begin
	det_s <= { det_s[0], sd_det_n };
	int_s <= { int_s[0], ft_int_n };
	miso_r <= miso;
	if (we && a == 4'd0) begin
		spi_dev <= wd[1:0];
		spi_cs <= wd[2];
		own_sd <= wd[3];
		own_ft <= wd[4];
		spi_half <= wd[15:8] == 8'd0 ? 8'd1 : wd[15:8];
	end
	if (spi_start_w || spi_start_n) begin
		spi_sh <= spi_start_w ? wd[7:0] : 8'hff;
		spi_mosi <= spi_start_w ? wd[7] : 1'b1;
		spi_bit <= 4'd15;
		spi_cnt <= 8'd0;
		spi_busy <= 1'b1;
	end else if (spi_busy) begin
		if (spi_cnt == spi_half) begin
			spi_cnt <= 8'd0;
			if (!spi_sck) spi_sck <= 1'b1;                // the rising edge: the slave takes MOSI
			else begin                                     // the falling edge: MISO taken, the next bit out
				spi_sck <= 1'b0;
				spi_in <= { spi_in[6:0], miso_r };
				spi_sh <= { spi_sh[6:0], 1'b1 };
				spi_mosi <= spi_sh[6];
			end
			if (spi_bit == 4'd0) spi_busy <= 1'b0;
			else spi_bit <= spi_bit - 4'd1;
		end else
			spi_cnt <= spi_cnt + 8'd1;
	end
	if (spi_busy && spi_bit == 4'd0 && spi_cnt == spi_half) spi_last <= { spi_in[6:0], miso_r };
	if (!spi_busy && !spi_start_w && !spi_start_n) spi_mosi <= 1'b1;
end

// ------------------------------------------------------------------ the uart of the ESP8266

reg [1:0] rx_s = 2'b11;
reg [15:0] rx_cnt = 16'd0;
reg [3:0] rx_bit = 4'd0;
reg [7:0] rx_sh = 8'd0;
reg rx_on = 1'b0;
reg rxf_wr = 1'b0;
reg [7:0] rxf_di = 8'd0;
reg rx_lost = 1'b0;
reg cts_hold = 1'b0;
reg rxf_clr = 1'b0;
wire rxf_full, rxf_empty, txf_full, txf_empty;
wire [7:0] rxf_do, txf_do;
wire [10:0] rxf_count;
wire [6:0] txf_count;
reg rxf_have = 1'b0;          // rxf_do holds a byte taken from the fifo, not read yet
reg rxf_rd = 1'b0;

kg_fifo #(.DW(8), .AW(10)) rx_fifo (
	.clk(clk), .reset(rxf_clr), .wr(rxf_wr), .din(rxf_di), .full(rxf_full),
	.rd(rxf_rd), .dout(rxf_do), .empty(rxf_empty), .count(rxf_count)
);
assign uart_cts = cts_hold | (rxf_count > 11'd768);   // active low at the ESP: 1 = stop

always @(posedge clk) begin
	rx_s <= { rx_s[0], uart_rx };
	rxf_wr <= 1'b0;
	if (!rx_on) begin
		if (!rx_s[1]) begin rx_on <= 1'b1; rx_cnt <= CLKS_PER_BIT / 2; rx_bit <= 4'd0; end
	end else if (rx_cnt == 16'd0) begin
		rx_cnt <= CLKS_PER_BIT - 1;
		if (rx_bit == 4'd0) begin
			if (rx_s[1]) rx_on <= 1'b0;                      // a glitch, not a start bit
			rx_bit <= 4'd1;
		end else if (rx_bit == 4'd9) begin
			rx_on <= 1'b0;
			if (rx_s[1]) begin                               // the stop bit
				if (rxf_full) rx_lost <= 1'b1;
				else begin rxf_wr <= 1'b1; rxf_di <= rx_sh; end
			end
		end else begin
			rx_sh <= { rx_s[1], rx_sh[7:1] };
			rx_bit <= rx_bit + 4'd1;
		end
	end else
		rx_cnt <= rx_cnt - 16'd1;
	// the byte for the CPU: the fifo output is held in rxf_do
	rxf_rd <= 1'b0;
	if (!rxf_have && !rxf_empty && !rxf_rd) begin rxf_rd <= 1'b1; end
	if (rxf_rd) rxf_have <= 1'b1;
	if (rd_go && a == 4'd4) begin
		if (rxf_have) rxf_have <= 1'b0;
		rx_lost <= 1'b0;
	end
	rxf_clr <= 1'b0;
	if (we && a == 4'd5) begin
		cts_hold <= wd[0];
		if (wd[1]) begin rxf_clr <= 1'b1; rxf_have <= 1'b0; end
	end
end

reg txf_rd = 1'b0;
kg_fifo #(.DW(8), .AW(6)) tx_fifo (
	.clk(clk), .reset(1'b0), .wr(we && a == 4'd4), .din(wd[7:0]), .full(txf_full),
	.rd(txf_rd), .dout(txf_do), .empty(txf_empty), .count(txf_count)
);
reg [15:0] tx_cnt = 16'd0;
reg [3:0] tx_bit = 4'd0;
reg [9:0] tx_sh = 10'h3ff;
reg tx_on = 1'b0, tx_load = 1'b0;
always @(posedge clk) begin
	txf_rd <= 1'b0;
	tx_load <= txf_rd;
	if (!tx_on && !txf_empty && !txf_rd && !tx_load) txf_rd <= 1'b1;
	if (tx_load) begin
		tx_sh <= { 1'b1, txf_do, 1'b0 };
		tx_on <= 1'b1;
		tx_bit <= 4'd0;
		tx_cnt <= 16'd0;
	end else if (tx_on) begin
		if (tx_cnt == 16'd0) begin
			uart_tx <= tx_sh[0];
			tx_sh <= { 1'b1, tx_sh[9:1] };
			tx_cnt <= CLKS_PER_BIT - 1;
			if (tx_bit == 4'd10) begin tx_on <= 1'b0; uart_tx <= 1'b1; end
			tx_bit <= tx_bit + 4'd1;
		end else
			tx_cnt <= tx_cnt - 16'd1;
	end
end

// ------------------------------------------------------------------ IDE PIO mode 0

localparam [5:0] T_SETUP = 6'd6, T_STROBE = 6'd30, T_HOLD = 6'd33, T_END = 6'd45;
reg [5:0] ide_t = 6'd0;
reg ide_busy = 1'b0, ide_wr = 1'b0;
reg [15:0] ide_q = 16'd0;
always @(posedge clk) begin
	if (we && a == 4'd10) ide_rst_n <= ~wd[0];
	if (we && a == 4'd8 && !ide_busy) begin
		ide_a <= wd[2:0];
		ide_cs_n <= wd[3] ? 2'b01 : 2'b10;
		ide_wr <= wd[4];
		ide_d_o <= wd[31:16];
		ide_d_oe <= wd[4];
		ide_busy <= 1'b1;
		ide_t <= 6'd0;
	end else if (ide_busy) begin
		ide_t <= ide_t + 6'd1;
		if (ide_t == T_SETUP) begin
			if (ide_wr) ide_wr_n <= 1'b0; else ide_rd_n <= 1'b0;
		end
		if (ide_t == T_STROBE) begin
			if (!ide_wr) ide_q <= ide_d_i;
			ide_rd_n <= 1'b1;
			ide_wr_n <= 1'b1;
		end
		if (ide_t == T_HOLD) begin ide_cs_n <= 2'b11; ide_d_oe <= 1'b0; end
		if (ide_t == T_END) ide_busy <= 1'b0;
	end
end

// ------------------------------------------------------------------ reads

always @(posedge clk) begin
	case (a)
		4'd0: rd <= { 16'd0, spi_half, 4'd0, int_s[1], ~det_s[1], mcu_ft_on, spi_busy };
		4'd1, 4'd2: rd <= { 24'd0, spi_last };
		4'd4: rd <= { 20'd0, rx_s[1], rx_lost, txf_full, rxf_have, rxf_do };
		4'd5: rd <= { 21'd0, rxf_count + { 10'd0, rxf_have } };
		4'd8: rd <= { 31'd0, ide_busy };
		4'd9: rd <= { 16'd0, ide_q };
		4'd12: rd <= { fdd_idx, 4'd0, fdd_s2 };
		4'd13: rd <= fdd_flux;
		default: rd <= 32'd0;
	endcase
end

endmodule
