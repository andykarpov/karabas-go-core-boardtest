`timescale 1ns / 1ps
`default_nettype none
/*-------------------------------------------------------------------------------------------------------------------
--
-- Karabas Board Test: the board test core of the Karabas Go Mini rev.G (docs/PLAN.md)
-- https://github.com/andykarpov/karabas-go-core-boardtest
--
-- The CPU (bt_soc: VexRiscv, its program in block RAM) draws the screen of tiles (bt_tiles, block RAM) over the
-- gradient of the boot menu (menu_compose); nothing of the UI is in the SRAM or the SDRAM. The top after the one of
-- karabas-go-core-menu.
--
-- Ф1: the base board set (tape in / out + monitor, MIDI_TX, the ADC, the audio peaks to the LED matrix of the MCU,
-- FT_CLK_OUT) and the hooks of the tests (bt_io, bt_audio). The SRAM and the SDRAM stay idle until Ф2.
--
-- @author Andy Karpov <andy.karpov@gmail.com>
-- EU, 2026
------------------------------------------------------------------------------------------------------------------*/

// HW_ID3: defined in karabas_minig.xst

module karabas_minig_top (
    //---------------------------
    input wire            CLK_50MHZ,

    //---------------------------
    inout wire            UART_RX,
    inout wire            UART_TX,
    inout wire            UART_CTS,

    //---------------------------
    output wire [20:0]    MA,
    inout wire [15:0]     MD,
    output wire [1:0]     MWR_N,
    output wire [1:0]     MRD_N,

    //---------------------------
    output wire [1:0]     SDR_BA,
    output wire [12:0]    SDR_A,
    output wire           SDR_CLK,
    output wire [1:0]     SDR_DQM,
    output wire           SDR_WE_N,
    output wire           SDR_CAS_N,
    output wire           SDR_RAS_N,
    inout wire [15:0]     SDR_DQ,

    //---------------------------
    output wire           SD_CS_N,
    output wire           SD_CLK,
    output wire           SD_DI,
    input wire            SD_DO,
    input wire            SD_DET_N,

    //---------------------------
    input wire [7:0]      VGA_R,
    input wire [7:0]      VGA_G,
    input wire [7:0]      VGA_B,
    input wire            VGA_HS,
    input wire            VGA_VS,

    output wire [3:0]     TMDS_P,
    output wire [3:0]     TMDS_N,

    //---------------------------
    output wire           FT_SPI_CS_N,
    output wire           FT_SPI_SCK,
    input wire            FT_SPI_MISO,
    output wire           FT_SPI_MOSI,
    input wire            FT_INT_N,
    input wire            FT_CLK,
    input wire            FT_DE,
    output wire           FT_CLK_OUT,

    //---------------------------
    output wire [2:0]     WA,
    output wire [1:0]     WCS_N,
    output wire           WRD_N,
    output wire           WWR_N,
    output wire           WRESET_N,
    inout wire [15:0]     WD,

    //---------------------------
    output wire           TAPE_OUT,
    input wire            TAPE_IN,

   //---------------------------
    output wire           DAC_BCK,
    output wire           DAC_WS,
    output wire           DAC_DAT,

    //---------------------------
    output wire           ADC_CLK,
    inout wire            ADC_BCK,
    inout wire            ADC_LRCK,
    input wire            ADC_DOUT,

    //---------------------------
    output wire           ESP32_SPI_CS_N,
    input wire            ESP32_PCM_BCK,
    input wire            ESP32_PCM_RLCK,
    input wire            ESP32_PCM_DAT,

    //---------------------------
    input wire            MCU_CS_N,
    input wire            MCU_SCK,
    input wire            MCU_MOSI,
    output wire           MCU_MISO,
    inout wire [5:0]      MCU_IO,  // [0] burst cs_n, [1] burst ready, [2] sd2 cs_n, [3] ft cs_n

    //---------------------------
    output wire           MIDI_TX,

    //---------------------------
    output wire           FLASH_CS_N,   // T3: the SDRAM ~CS on the Mini G2 (no flash chip is fitted on any board)
    input wire            FLASH_DO,
    output wire           FLASH_DI,
    output wire           FLASH_SCK,
    output wire           FLASH_WP_N,
    output wire           FLASH_HOLD_N
);

// the SPI flash: never fitted (its group shows N/A)
assign FLASH_DI           = 1'b1;
assign FLASH_SCK          = 1'b0;
assign FLASH_WP_N         = 1'b1;
assign FLASH_HOLD_N       = 1'b1;

// system clocks
wire clk_sys, clk_sys_pll, clk_8mhz, clk_cpu;
wire locked, areset;

// VCO 50 * 18 / 2 = 450 MHz, phase 0: clk_sys 40.91 MHz (/11: the 800 x 600 video, HDMI, audio), clk_8 8.18 MHz
// (/55: FT_CLK_OUT, the base of DCM_CLKGEN in hdmi_pll), clk_cpu 75 MHz (/6: the CPU, the MCU link). The PLL output
// of clk_sys has no BUFG in kg_pll: it drives the BUFG of clk_sys and the BUFGMUX of hdmi_pll.
wire [5:0] pll_clk;
kg_pll #(
    .DIVCLK_DIVIDE(2), .CLKFBOUT_MULT(18),
    .CLKOUT0_DIVIDE(11), .CLKOUT1_DIVIDE(55), .CLKOUT2_DIVIDE(6),
    .BUFG_MASK(6'b000110)
) pll (
    .clk_in              (CLK_50MHZ),
    .rst                 (1'b0),
    .clk_out             (pll_clk),
    .locked              (locked)
);
assign clk_sys_pll = pll_clk[0];
assign clk_8mhz = pll_clk[1];
assign clk_cpu = pll_clk[2];
BUFG bufg_clk_sys(.I(clk_sys_pll), .O(clk_sys));
assign areset = ~locked;

// the reset of the clk_cpu side, synchronous to it
reg [1:0] areset_cpu = 2'b11;
always @(posedge clk_cpu) areset_cpu <= { areset_cpu[0], areset };
wire reset_cpu = areset_cpu[1];

// ft clk 8mhz out
ODDR2 u_ft_clk (.Q(FT_CLK_OUT), .C0(clk_8mhz), .C1(~clk_8mhz), .CE(1'b1), .D0(1'b1), .D1(1'b0), .R(1'b0), .S(1'b0));

// MCU_SCK on a global buffer; the SCK passed to the FT812 and SD2 is taken before the BUFG (a BUFG drives clock
// pins only)
wire mcu_sck_ibufg, mcu_sck;
IBUFG ibufg_mcu_sck(.I(MCU_SCK), .O(mcu_sck_ibufg));
BUFG bufg_mcu_sck(.I(mcu_sck_ibufg), .O(mcu_sck));

// the FT812 and the SD2 slot passed through to the MCU (as the menu; the tests of Ф3 take them)
reg [1:0] ft_vga_s = 2'b00, dvi_only_s = 2'b00;
always @(posedge clk_sys) begin
    ft_vga_s <= { ft_vga_s[0], mcu_ft_vga_on };
    dvi_only_s <= { dvi_only_s[0], dvi_only };
end
wire vdac2_sel = ft_vga_s[1];
// the SPI master of bt_per (Ф3) has them while the CPU asks: the SD2 slot (the firmware uses it only under the boot
// core), the FT812 bus with the ESP32 on it (never while the MCU has it, FT_SPI_ON)
wire per_own_sd, per_own_ft, per_sck, per_mosi, per_sd_cs_n, per_ft_cs_n, per_esp32_cs_n, mcu_sd_mosi, mcu_sd_cs_n;
wire per_ft = per_own_ft & ~mcu_ft_spi_on;
assign FT_SPI_CS_N = mcu_ft_spi_on ? mcu_ft_cs_n : per_ft ? per_ft_cs_n : 1'b1;
assign FT_SPI_SCK = mcu_ft_spi_on ? mcu_sck_ibufg : per_ft ? per_sck : 1'b0;
assign FT_SPI_MOSI = mcu_ft_spi_on ? mcu_ft_mosi : per_ft ? per_mosi : 1'b1;
assign ESP32_SPI_CS_N = per_ft ? per_esp32_cs_n : 1'b1;
assign SD_CLK = per_own_sd ? per_sck : mcu_sck_ibufg;
assign SD_DI = per_own_sd ? per_mosi : mcu_sd_mosi;
assign SD_CS_N = per_own_sd ? per_sd_cs_n : mcu_sd_cs_n;

wire ft_clk_ibufg, ft_clk_int;
IBUFG ibufg_ft_clk(.I(FT_CLK), .O(ft_clk_ibufg));
BUFG bufg_ft_clk(.I(ft_clk_ibufg), .O(ft_clk_int));

// the picture: the bitmap OSD over the screen of the CPU (below)
wire [7:0] osd_r, osd_g, osd_b;
wire video_hs, video_vs, video_de;

// hdmi
hdmi_top #(.SAMPLERATE(48000)) hdmi_top(
    .clk                (ft_clk_int),
    .clk_ref            (clk_sys),
    .clk_ref_pll        (clk_sys_pll),
    .clk_8              (clk_8mhz),
    .reset              (areset),

    .vga_rgb            ({osd_r[7:0], osd_g[7:0], osd_b[7:0]}),
    .vga_hs             (video_hs),
    .vga_vs             (video_vs),
    .vga_de             (video_de),

    .ft_rgb             ({VGA_R[7:0], VGA_G[7:0], VGA_B[7:0]}),
    .ft_hs              (VGA_HS),
    .ft_vs              (VGA_VS),
    .ft_de              (FT_DE),

    .ft_sel             (vdac2_sel),

    .audio_en           (~dvi_only_s[1]),
    .audio_l            (snd_l),
    .audio_r            (snd_r),

    .tmds_p             (TMDS_P),
    .tmds_n             (TMDS_N),

    .freq               ()
);

// ------- PCM1808 ADC: the line in and the SAM2695 ---------
wire signed [23:0] adc_l, adc_r;
i2s_transceiver adc(
    .reset_n            (~areset),
    .mclk               (clk_sys),
    .sclk               (ADC_BCK),
    .ws                 (ADC_LRCK),
    .sd_tx              (),
    .sd_rx              (ADC_DOUT),
    .l_data_tx          (24'b0),
    .r_data_tx          (24'b0),
    .l_data_rx          (adc_l),
    .r_data_rx          (adc_r)
);
ODDR2 oddr_adc2(.Q(ADC_CLK), .C0(clk_sys), .C1(~clk_sys), .CE(1'b1), .D0(1'b1), .D1(1'b0), .R(1'b0), .S(1'b0));

// ------- the sound of the tests: generators, the ADC, the tape monitor -> the DAC and HDMI ---------
wire signed [15:0] snd_l, snd_r, smp_adc_l, smp_adc_r, smp_mix_l, smp_mix_r;
wire smp_t, tape_out;
wire [15:0] aud_ctl;
wire [31:0] aud_inc_l, aud_inc_r;
bt_audio #(.CLK_HZ(40909090)) audio (
    .clk(clk_sys), .ctl(aud_ctl), .inc_l(aud_inc_l), .inc_r(aud_inc_r),
    .adc_l(adc_l[23:8]), .adc_r(adc_r[23:8]), .tape_in(TAPE_IN),
    .out_l(snd_l), .out_r(snd_r), .tape_out(tape_out),
    .smp_t(smp_t), .smp_adc_l(smp_adc_l), .smp_adc_r(smp_adc_r), .smp_mix_l(smp_mix_l), .smp_mix_r(smp_mix_r)
);
assign TAPE_OUT = tape_out;

kg_pcm5102 #(.DIV_BITS(2)) PCM5102(
    .clk                (clk_sys),
    .reset              (areset),
    .left               (snd_l),
    .right              (snd_r),
    .din                (DAC_DAT),
    .bck                (DAC_BCK),
    .lrck               (DAC_WS)
);

//---------- MCU ------------
// MCU link v2 (karabas-go-firmware docs/link_v2.md, kg_mcu2): INT on IO1, the bitmap OSD, the FT812 and the SD2
// slot passed through, AUX (the LED matrix: the frames of the CPU and the audio peaks), the buttons (BTNS: the
// board ones with the action "core" of core.json, the matrix ones), RPC (SYS, CFG for the gradient colours of
// karabas.ini, CORES: the firmware gives the karabas.ini of CFG to a core whose HELLO lists CORES)
wire mcu_ft_spi_on, mcu_ft_vga_on, mcu_ft_mosi, mcu_ft_cs_n;
wire dvi_only;
wire mcu_int;
assign MCU_IO[1] = mcu_int;
assign MCU_IO[0] = 1'bz;
assign MCU_IO[5:2] = 4'bzzzz;

wire [55:0] kbd;
wire kbd_wr, ms_wr;
wire [7:0] ms_x, ms_y, ms_z, ms_b;
wire [15:0] joy_l, joy_r;
wire [1:0] ld_kind;
wire ld_active, ld_end, ld_wr, ld_ready;
wire [31:0] ld_addr;
wire [7:0] ld_data;
wire [10:0] rpc_a;
wire rpc_we;
wire [7:0] rpc_wd, rpc_rd;
wire osd_wr, osd_en, osd_zoom;
wire [11:0] osd_waddr;
wire [7:0] osd_wdata;
wire [23:0] osd_fg, osd_bg;
wire [1:0] osd_alpha, osd_place;
wire [4:0] osd_px, osd_ph;
wire [3:0] osd_py;
wire [5:0] osd_pw;
wire [5:0] btns;
wire [23:0] matrix_cmd;
wire matrix_cmd_wr;

kg_mcu2 #(
    .CAPS(16'h0000), .SW_N(2), .CS_FILTER(3),
    .ENABLE_OSD(1), .ENABLE_PORTS(4'b0000),
    .ENABLE_FT(1), .ENABLE_SD2(1), .ENABLE_AUX(1),
    .ENABLE_RPC(1), .RPC_SERVICES(16'h0007)    // SYS, CFG, CORES (karabas-go-firmware docs/link_v2.md 5.9)
) mcu(
    .CLK(clk_cpu), .N_RESET(~reset_cpu),
    .MCU_SCK(mcu_sck), .MCU_MOSI(MCU_MOSI), .MCU_MISO(MCU_MISO), .MCU_SS(MCU_CS_N), .MCU_INT(mcu_int),
    .MCU_SPI_FT_SS(MCU_IO[3]), .MCU_SPI_SD2_SS(MCU_IO[2]),

    .SW(), .SW_WR(), .SW_INDEX(), .SW_VALUE(), .HW_ID(), .DVI_ONLY(dvi_only), .CORE_BUILD(), .FW_BUILD(),
    .BUSY(), .BTNS(btns), .RELOAD_REQ(1'b0), .RELOAD_CORE(8'd0),
    .KBD(kbd), .KBD_WR(kbd_wr), .PS2_CODE(), .PS2_KIND(), .PS2_WR(), .MS_X(ms_x), .MS_Y(ms_y), .MS_Z(ms_z),
    .MS_B(ms_b), .MS_WR(ms_wr), .JOY_L(joy_l), .JOY_R(joy_r), .JOY_WR(), .PS2_CMD(16'h0000), .PS2_CMD_WR(1'b0),
    .LD_KIND(ld_kind), .LD_SLOT(), .LD_SIZE(), .LD_START(), .LD_ACTIVE(ld_active), .LD_END(ld_end),
    .LD_ADDR(ld_addr), .LD_DATA(ld_data), .LD_WR(ld_wr), .LD_READY(ld_ready),

    .OSD_WR(osd_wr), .OSD_WADDR(osd_waddr), .OSD_WDATA(osd_wdata), .OSD_EN(osd_en),
    .OSD_FG(osd_fg), .OSD_BG(osd_bg), .OSD_ALPHA(osd_alpha), .OSD_PX(osd_px), .OSD_PY(osd_py),
    .OSD_PW(osd_pw), .OSD_PH(osd_ph), .OSD_PLACE(osd_place), .OSD_ZOOM(osd_zoom),

    .SDC_IMG_MOUNTED(), .SDC_IMG_SIZE(), .SDC_RD(8'd0), .SDC_WR(8'd0), .SDC_SECTOR(32'd0),
    .SDC_BUSY(), .SDC_DONE(), .SDC_ERROR(), .SDC_SLOT(), .SDC_BYTE_IN_STROBE(), .SDC_BYTE_ADDR(),
    .SDC_BYTE_IN_DATA(), .SDC_BYTE_OUT_DATA(8'd0), .IMG_WP(), .IMG_PHYS_WR(), .IMG_PHYS(),
    .IMG_FLUSH(), .FDD_CHNG_WR(), .FDD_CHNG(), .FDD_CHNG_REQ(1'b0), .FDD_CHNG_DS(2'd0), .IMG_ACTIVE(),

    .PORT_OUT_DATA(), .PORT_OUT_WR(), .PORT_OUT_READY(4'b1111),
    .PORT_IN_DATA(32'd0), .PORT_IN_WR(4'b0000),
    .PORT_IN_FULL(), .PORT_CFG(128'd0), .PORT_STAT(), .PORT_STAT_WR(),

    .FT_SPI_ON(mcu_ft_spi_on), .FT_VGA_ON(mcu_ft_vga_on), .FT_RESET(), .FT_SCK(), .FT_MOSI(mcu_ft_mosi),
    .FT_MISO(FT_SPI_MISO), .FT_CS_N(mcu_ft_cs_n),
    .MATRIX_CMD(matrix_cmd), .MATRIX_CMD_WR(matrix_cmd_wr), .DEBUG_ADDR(16'd0), .DEBUG_DATA(16'd0),
    .RPC_A(rpc_a), .RPC_WE(rpc_we), .RPC_WD(rpc_wd), .RPC_RD(rpc_rd),
    .SD2_SCK(), .SD2_MOSI(mcu_sd_mosi), .SD2_MISO(SD_DO), .SD2_CS_N(mcu_sd_cs_n)
);

//--------- the test computer ---------
wire cell_we, pal_we, atl_we, atl_4b, show, frame_t;
wire [11:0] cell_a;
wire [17:0] cell_wd, cell_rd;
wire [3:0] pal_wa;
wire [27:0] pal_wd;
wire [15:0] atl_a;
wire [7:0] atl_d;
wire [31:0] cursor;
wire [23:0] grad_top, grad_bot;

bt_soc soc (
    .clk(clk_cpu), .reset(reset_cpu),
    .ld_kind(ld_kind), .ld_active(ld_active), .ld_end(ld_end), .ld_addr(ld_addr), .ld_data(ld_data),
    .ld_wr(ld_wr), .ld_ready(ld_ready),
    .kbd(kbd), .kbd_wr(kbd_wr), .ms_x(ms_x), .ms_y(ms_y), .ms_z(ms_z), .ms_b(ms_b), .ms_wr(ms_wr),
    .joy_l(joy_l), .joy_r(joy_r),
    .cell_we(cell_we), .cell_a(cell_a), .cell_wd(cell_wd), .cell_rd(cell_rd),
    .pal_we(pal_we), .pal_wa(pal_wa), .pal_wd(pal_wd),
    .atl_we(atl_we), .atl_4b(atl_4b), .atl_a(atl_a), .atl_d(atl_d),
    .cursor(cursor), .grad_top(grad_top), .grad_bot(grad_bot), .show(show), .frame_t(frame_t),
    .bio_we(bio_we), .bio_rd_go(bio_rd_go), .bio_a(bio_a), .bio_wd(bio_wd), .bio_rd(bio_rd),
    .rpc_a(rpc_a), .rpc_we(rpc_we), .rpc_wd(rpc_wd), .rpc_rd(rpc_rd),
    .dbg_uart(), .dbg_uart_wr(), .cpu_run()
);

// the window 0x800..0xffc of bt_soc: 0x800 bt_io, 0x900 bt_sram (words 0x40..0x47)
wire bio_we, bio_rd_go;
wire [8:0] bio_a;
wire [31:0] bio_wd, bio_rd, io_rd, sram_rd;
wire sel_sram = bio_a[8:3] == 6'h08;
wire sel_sdram = bio_a[8:3] == 6'h10;          // 0xa00 bt_sdram
wire sel_per = bio_a[8:4] == 5'h0c;            // 0xb00 bt_per
wire sel_io = ~sel_sram & ~sel_sdram & ~sel_per;
wire [31:0] sdram_rd, per_rd;
assign bio_rd = sel_sram ? sram_rd : sel_sdram ? sdram_rd : sel_per ? per_rd : io_rd;
bt_io #(.CLK_HZ(75000000)) io (
    .clk(clk_cpu), .we(bio_we & sel_io), .a(bio_a), .wd(bio_wd), .rd(io_rd), .rd_go(bio_rd_go & sel_io),
    .btns(btns), .dvi_only(dvi_only), .matrix_cmd(matrix_cmd), .matrix_cmd_wr(matrix_cmd_wr),
    .aud_ctl(aud_ctl), .aud_inc_l(aud_inc_l), .aud_inc_r(aud_inc_r),
    .smp_t(smp_t), .smp_adc_l(smp_adc_l), .smp_adc_r(smp_adc_r), .smp_mix_l(smp_mix_l), .smp_mix_r(smp_mix_r),
    .tape_in(TAPE_IN), .midi_tx(MIDI_TX)
);

//--------- SD2, FT812, ESP32, ESP8266, CF / IDE (Ф3) ---------
wire [15:0] ide_do;
wire ide_oe;
bt_per #(.CLKS_PER_BIT(651)) per (
    .clk(clk_cpu), .we(bio_we & sel_per), .a(bio_a[3:0]), .wd(bio_wd), .rd(per_rd), .rd_go(bio_rd_go & sel_per),
    .own_sd(per_own_sd), .own_ft(per_own_ft), .mcu_ft_on(mcu_ft_spi_on), .spi_sck(per_sck), .spi_mosi(per_mosi),
    .sd_cs_n(per_sd_cs_n), .ft_cs_n(per_ft_cs_n), .esp32_cs_n(per_esp32_cs_n), .sd_miso(SD_DO), .ft_miso(FT_SPI_MISO),
    .sd_det_n(SD_DET_N), .ft_int_n(FT_INT_N),
    .uart_rx(UART_RX), .uart_tx(UART_TX), .uart_cts(UART_CTS),
    .ide_a(WA), .ide_cs_n(WCS_N), .ide_rd_n(WRD_N), .ide_wr_n(WWR_N), .ide_rst_n(WRESET_N), .ide_d_i(WD),
    .ide_d_o(ide_do), .ide_d_oe(ide_oe), .fdd_out(), .fdd_in(4'd0)
);
assign WD = ide_oe ? ide_do : 16'hzzzz;

//--------- the SRAM tester (Ф2) ---------
wire [15:0] sram_do, sram_oe_n;
bt_sram sram (
    .clk(clk_cpu), .we(bio_we & sel_sram), .a(bio_a[2:0]), .wd(bio_wd), .rd(sram_rd),
    .MA(MA), .MD_I(MD), .MD_O(sram_do), .MD_OE_N(sram_oe_n), .MWR_N(MWR_N), .MRD_N(MRD_N)
);
genvar gi;
generate for (gi = 0; gi < 16; gi = gi + 1) begin : g_md
    assign MD[gi] = sram_oe_n[gi] ? 1'bz : sram_do[gi];
end endgenerate

//--------- the SDRAM tester (Ф2) ---------
// its clock: a DCM_CLKGEN on clk_cpu, CLKFX = 75 MHz x M / D set by the CPU over PROG (100 .. 140 MHz: the chips of
// the boards are slow ones, the user 2026-11-22); the start value 28 / 15 (140 MHz) is the highest of the table: the
// timing is analysed there (CLKFX_MD_MAX). The SDRAM gets
// CLKFX180 through an ODDR2 (as the old sdram-test core). On the Mini G2 the SDRAM ~CS is T3 (FLASH_CS_N).
wire sd_fx, sd_fx180, clk_sd, clk_sd180, sd_locked, sd_progdone, sd_progen, sd_progdata, sd_dcm_rst;
DCM_CLKGEN #(
    .CLKFXDV_DIVIDE(2), .CLKFX_DIVIDE(15), .CLKFX_MULTIPLY(28), .CLKFX_MD_MAX(1.87), .CLKIN_PERIOD(13.333),
    .SPREAD_SPECTRUM("NONE"), .STARTUP_WAIT("FALSE")
) sd_dcm (
    .CLKIN(clk_cpu), .CLKFX(sd_fx), .CLKFX180(sd_fx180), .CLKFXDV(), .LOCKED(sd_locked), .PROGDONE(sd_progdone),
    .STATUS(), .FREEZEDCM(1'b0), .PROGCLK(clk_cpu), .PROGDATA(sd_progdata), .PROGEN(sd_progen), .RST(sd_dcm_rst | areset)
);
BUFG bufg_sd(.I(sd_fx), .O(clk_sd));
BUFG bufg_sd180(.I(sd_fx180), .O(clk_sd180));
ODDR2 #(.DDR_ALIGNMENT("NONE"), .INIT(1'b0), .SRTYPE("SYNC")) sd_clk_oddr (
    .Q(SDR_CLK), .C0(clk_sd180), .C1(~clk_sd180), .CE(1'b1), .D0(1'b1), .D1(1'b0), .R(1'b0), .S(1'b0));

wire [15:0] sd_do, sd_oe_n;
bt_sdram sdram (
    .clk_cpu(clk_cpu), .we(bio_we & sel_sdram), .a(bio_a[2:0]), .wd(bio_wd), .rd(sdram_rd),
    .dcm_progen(sd_progen), .dcm_progdata(sd_progdata), .dcm_rst(sd_dcm_rst), .dcm_progdone(sd_progdone),
    .dcm_locked(sd_locked),
    .clk(clk_sd), .SDR_CS_N(FLASH_CS_N), .SDR_BA(SDR_BA), .SDR_A(SDR_A), .SDR_RAS_N(SDR_RAS_N), .SDR_CAS_N(SDR_CAS_N),
    .SDR_WE_N(SDR_WE_N), .SDR_DQM(SDR_DQM), .DQ_I(SDR_DQ), .DQ_O(sd_do), .DQ_OE_N(sd_oe_n)
);
generate for (gi = 0; gi < 16; gi = gi + 1) begin : g_sdq
    assign SDR_DQ[gi] = sd_oe_n[gi] ? 1'bz : sd_do[gi];
end endgenerate

//--------- video ---------
wire [27:0] t_rgba;
wire t_on, t_hs, t_vs, t_de;
bt_tiles tiles (
    .clk_cpu(clk_cpu),
    .cell_we(cell_we), .cell_a(cell_a), .cell_wd(cell_wd), .cell_rd(cell_rd),
    .pal_we(pal_we), .pal_wa(pal_wa), .pal_wd(pal_wd),
    .atl_we(atl_we), .atl_4b(atl_4b), .atl_a(atl_a), .atl_d(atl_d),
    .cursor(cursor), .show(show), .frame_t(frame_t),
    .clk_pix(clk_sys), .rgba(t_rgba), .show_o(t_on), .hs(t_hs), .vs(t_vs), .de(t_de)
);

// the gradient of the boot menu (the [ft812] colours of karabas.ini, written by the CPU), black before the CPU
// shows its screen; the tiles blended over it
wire [23:0] bg_rgb;
wire bg_hs, bg_vs, bg_de;
menu_compose compose(
    .clk(clk_sys), .rgba_i(t_rgba), .show_i(t_on), .hs_i(t_hs), .vs_i(t_vs), .de_i(t_de),
    .glow_i(24'd0), .grad_top(grad_top), .grad_bot(grad_bot),
    .rgb_o(bg_rgb), .hs_o(bg_hs), .vs_o(bg_vs), .de_o(bg_de)
);

//--------- OSD --------------
// the bitmap OSD of the firmware over it all, scale 2 (512 x 256 of 800 x 600)
kg_l2_osd_overlay #(.SCALE(2)) overlay(
    .wclk(clk_cpu), .osd_wr(osd_wr), .osd_waddr(osd_waddr), .osd_wdata(osd_wdata), .osd_en(osd_en),
    .osd_fg(osd_fg), .osd_bg(osd_bg), .osd_alpha(osd_alpha), .osd_px(osd_px), .osd_py(osd_py),
    .osd_pw(osd_pw), .osd_ph(osd_ph), .osd_place(osd_place), .osd_zoom(osd_zoom),
    .clk(clk_sys), .rgb_i(bg_rgb), .hs_i(bg_hs), .vs_i(bg_vs), .de_i(bg_de),
    .rgb_o({ osd_r, osd_g, osd_b }), .hs_o(video_hs), .vs_o(video_vs), .de_o(video_de)
);

endmodule
