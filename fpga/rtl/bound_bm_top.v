// bound_bm_top.v - BOUND (BoundHash) top level for one FPGA of a BlackMiner hash board
// (F1 Ultra XC7K420T, F2 XC7VX415T, F1 Plus XC7K325T: the pins are in the xdc). From prox_f2_top.v.
//
// 25 MHz oscillator -> two MMCMs:
//   u_mmcm_c: clk_ctl  = 25 * 40 / 10 = 100 MHz (UART, XADC, control), fixed
//   u_mmcm_h: clk_hash = 25 * MMCM_MULT / MMCM_DIV0 at power-up (build.tcl), and
//             reprogrammable at run time through its DRP (command CLK, mmcm_drp.v)
`default_nettype none
module bound_bm_top #(
    parameter integer NCORE      = 7,
    parameter integer RDLAT      = 1,
    parameter integer BM2        = 0,
    parameter integer MULLAT     = 2,
    parameter integer HASH_MHZ   = 95,
    parameter real    MMCM_MULT  = 38.0,   // VCO 950 MHz
    parameter real    MMCM_DIV0  = 10.0,   // 95 MHz
    parameter integer MMCM_DIV1  = 10,     // unused (control clock has its own MMCM)
    parameter integer BAUD       = 115200
) (
    input  wire       osc_clk,
    input  wire       CI,        // commands from upstream (CPU or previous FPGA)
    output wire       CO,        // commands to the next FPGA
    input  wire       RI,        // responses from the next FPGA
    output wire       RO,        // responses to upstream
    input  wire       BI,
    output wire       BO,
    input  wire [2:0] ADDR,
    output wire [1:0] Xil_LED
);

// ------------------------------------------------------------------ clocks
wire clk_in, clk_osc, fbc, fbc_buf, cc0, clk_ctl, locked_c;
wire fbh, ch0, clk_hash, locked_h;
wire [6:0]  drp_addr; wire [15:0] drp_di, drp_do; wire drp_en, drp_we, drp_rdy, mmcm_h_rst;
IBUF u_ibuf (.I(osc_clk), .O(clk_in));
BUFG u_bosc (.I(clk_in), .O(clk_osc));

MMCME2_BASE #(
    .CLKIN1_PERIOD(40.0), .DIVCLK_DIVIDE(1), .CLKFBOUT_MULT_F(40.0),
    .CLKOUT0_DIVIDE_F(10.0), .BANDWIDTH("OPTIMIZED"), .STARTUP_WAIT("FALSE")
) u_mmcm_c (
    .CLKIN1(clk_osc), .CLKFBIN(fbc_buf), .CLKFBOUT(fbc), .CLKFBOUTB(),
    .CLKOUT0(cc0), .CLKOUT0B(), .CLKOUT1(), .CLKOUT1B(),
    .CLKOUT2(), .CLKOUT2B(), .CLKOUT3(), .CLKOUT3B(), .CLKOUT4(), .CLKOUT5(), .CLKOUT6(),
    .LOCKED(locked_c), .PWRDWN(1'b0), .RST(1'b0)
);
BUFG u_bfbc (.I(fbc), .O(fbc_buf));
BUFG u_bc   (.I(cc0), .O(clk_ctl));

MMCME2_ADV #(
    .CLKIN1_PERIOD(40.0), .DIVCLK_DIVIDE(1), .CLKFBOUT_MULT_F(MMCM_MULT),
    .CLKOUT0_DIVIDE_F(MMCM_DIV0), .BANDWIDTH("OPTIMIZED"), .COMPENSATION("INTERNAL"),
    .STARTUP_WAIT("FALSE")
) u_mmcm_h (
    .CLKIN1(clk_osc), .CLKIN2(1'b0), .CLKINSEL(1'b1),
    .CLKFBIN(fbh), .CLKFBOUT(fbh), .CLKFBOUTB(),
    .CLKOUT0(ch0), .CLKOUT0B(), .CLKOUT1(), .CLKOUT1B(),
    .CLKOUT2(), .CLKOUT2B(), .CLKOUT3(), .CLKOUT3B(), .CLKOUT4(), .CLKOUT5(), .CLKOUT6(),
    .DCLK(clk_ctl), .DADDR(drp_addr), .DI(drp_di), .DO(drp_do), .DEN(drp_en), .DWE(drp_we), .DRDY(drp_rdy),
    .PSCLK(1'b0), .PSEN(1'b0), .PSINCDEC(1'b0), .PSDONE(),
    .LOCKED(locked_h), .CLKINSTOPPED(), .CLKFBSTOPPED(), .PWRDWN(1'b0), .RST(mmcm_h_rst)
);
BUFG u_bh (.I(ch0), .O(clk_hash));

// ------------------------------------------------------------------ core
bound_fpga #(
    .NCORE(NCORE), .RDLAT(RDLAT), .BM2(BM2), .MULLAT(MULLAT), .HASH_MHZ(HASH_MHZ), .CTL_HZ(100000000),
    .CLKS_PER_BIT((100000000 + BAUD / 2) / BAUD)
) u_fpga (
    .clk_ctl(clk_ctl), .clk_hash(clk_hash), .locked_c(locked_c), .locked_h(locked_h),
    .drp_addr(drp_addr), .drp_di(drp_di), .drp_do(drp_do), .drp_en(drp_en), .drp_we(drp_we),
    .drp_rdy(drp_rdy), .mmcm_h_rst(mmcm_h_rst),
    .ci(CI), .co(CO), .ri(RI), .ro(RO), .bi(BI), .bo(BO),
    .addr_pins(ADDR), .led(Xil_LED), .hashing()
);

endmodule
`default_nettype wire
