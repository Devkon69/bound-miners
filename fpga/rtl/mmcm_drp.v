// mmcm_drp.v - reprogram the hash-clock MMCM (MMCME2_ADV) at run time through
// its dynamic reconfiguration port.
//
//   f_hash = 25 MHz * M / O        (DIVCLK = 1, integer M and O)
//
// Register values follow the Analog Devices axi_clkgen Linux driver
// (drivers/clk/clk-axi-clkgen.c, GPL-2.0, Lars-Peter Clausen) for 7-series
// MMCMs with BANDWIDTH = "OPTIMIZED": lock and filter tables indexed by M-1,
// read-modify-write of 13 registers while the MMCM is held in reset.
//
// The caller checks the ranges (24 <= M <= 57, 2 <= O <= 64); the hash-clock
// domain is reset while the MMCM relocks.
`default_nettype none
module mmcm_drp (
    input  wire        clk,            // DCLK (control clock, independent of this MMCM)
    input  wire        rst,
    input  wire        start,          // one-cycle request
    input  wire [5:0]  m,              // CLKFBOUT_MULT
    input  wire [6:0]  o,              // CLKOUT0_DIVIDE
    output reg         busy,
    // MMCM ports
    output reg  [6:0]  daddr,
    output reg  [15:0] di,
    input  wire [15:0] dout,
    output reg         den,
    output reg         dwe,
    input  wire        drdy,
    output reg         mmcm_rst,
    input  wire        locked
);

function [31:0] lock_lookup;           // index = M - 1
    input [5:0] i;
    case (i)
        6'd0:  lock_lookup = 32'h060603e8; 6'd1:  lock_lookup = 32'h060603e8;
        6'd2:  lock_lookup = 32'h080803e8; 6'd3:  lock_lookup = 32'h0b0b03e8;
        6'd4:  lock_lookup = 32'h0e0e03e8; 6'd5:  lock_lookup = 32'h111103e8;
        6'd6:  lock_lookup = 32'h131303e8; 6'd7:  lock_lookup = 32'h161603e8;
        6'd8:  lock_lookup = 32'h191903e8; 6'd9:  lock_lookup = 32'h1c1c03e8;
        6'd10: lock_lookup = 32'h1f1f0384; 6'd11: lock_lookup = 32'h1f1f0339;
        6'd12: lock_lookup = 32'h1f1f02ee; 6'd13: lock_lookup = 32'h1f1f02bc;
        6'd14: lock_lookup = 32'h1f1f028a; 6'd15: lock_lookup = 32'h1f1f0271;
        6'd16: lock_lookup = 32'h1f1f023f; 6'd17: lock_lookup = 32'h1f1f0226;
        6'd18: lock_lookup = 32'h1f1f020d; 6'd19: lock_lookup = 32'h1f1f01f4;
        6'd20: lock_lookup = 32'h1f1f01db; 6'd21: lock_lookup = 32'h1f1f01c2;
        6'd22: lock_lookup = 32'h1f1f01a9; 6'd23: lock_lookup = 32'h1f1f0190;
        6'd24: lock_lookup = 32'h1f1f0190; 6'd25: lock_lookup = 32'h1f1f0177;
        6'd26: lock_lookup = 32'h1f1f015e; 6'd27: lock_lookup = 32'h1f1f015e;
        6'd28: lock_lookup = 32'h1f1f0145; 6'd29: lock_lookup = 32'h1f1f0145;
        6'd30: lock_lookup = 32'h1f1f012c; 6'd31: lock_lookup = 32'h1f1f012c;
        6'd32: lock_lookup = 32'h1f1f012c; 6'd33: lock_lookup = 32'h1f1f0113;
        6'd34: lock_lookup = 32'h1f1f0113; 6'd35: lock_lookup = 32'h1f1f0113;
        default: lock_lookup = 32'h1f1f00fa;
    endcase
endfunction

function [31:0] filter_lookup;         // index = M - 1
    input [5:0] i;
    if      (i == 6'd0)  filter_lookup = 32'h01001990;
    else if (i == 6'd1)  filter_lookup = 32'h01001190;
    else if (i == 6'd2)  filter_lookup = 32'h01009890;
    else if (i == 6'd3)  filter_lookup = 32'h01001890;
    else if (i == 6'd4)  filter_lookup = 32'h01008890;
    else if (i <= 6'd8)  filter_lookup = 32'h01009090;
    else if (i <= 6'd11) filter_lookup = 32'h01000890;
    else if (i == 6'd12) filter_lookup = 32'h08009090;
    else if (i <= 6'd22) filter_lookup = 32'h01001090;
    else if (i <= 6'd36) filter_lookup = 32'h01008090;
    else if (i <= 6'd46) filter_lookup = 32'h08001090;
    else                 filter_lookup = 32'h08008090;
endfunction

// divider -> {reg1 value, reg2 value}: high/low time, edge, no-count (integer divide)
function [31:0] div_regs;
    input [6:0] d;
    reg [6:0] hi, lo; reg ed, nc;
    begin
        if (d == 7'd1) begin hi = 7'd0; lo = 7'd0; ed = 1'b0; nc = 1'b1; end
        else begin hi = d >> 1; ed = d[0]; lo = d - hi; nc = 1'b0; end
        div_regs = {8'd0, ed, nc, 6'd0, 4'd0, hi[5:0], lo[5:0]};
    end
endfunction

localparam integer NREG = 13;
reg [6:0]  r_addr [0:NREG-1];
reg [15:0] r_val  [0:NREG-1];
reg [15:0] r_mask [0:NREG-1];

reg [3:0]  idx;
reg [2:0]  st;
reg [7:0]  wait_cnt;
reg [19:0] lock_tmo;
localparam [2:0] S_IDLE = 3'd0, S_RST = 3'd1, S_RD = 3'd2, S_RDW = 3'd3, S_WR = 3'd4,
                 S_WRW = 3'd5, S_LOCK = 3'd6;

wire [31:0] lk = lock_lookup(m - 6'd1);
wire [31:0] fl = filter_lookup(m - 6'd1);
wire [31:0] dvo = div_regs(o);
wire [31:0] dvm = div_regs({1'b0, m});

always @(posedge clk) begin
    den <= 1'b0; dwe <= 1'b0;
    if (rst) begin
        st <= S_IDLE; busy <= 1'b0; mmcm_rst <= 1'b0; idx <= 4'd0;
    end else case (st)
        S_IDLE: if (start) begin
            // POWER, CLKOUT0 (1, 2, frac phase in CLKOUT5_2), DIVCLK, CLKFBOUT (1, 2, CLKOUT6_2),
            // LOCK1..3, FILTER1..2  (address, value, mask written bits)
            r_addr[0]  <= 7'h28; r_val[0]  <= 16'h0000;            r_mask[0]  <= 16'h9800;
            r_addr[1]  <= 7'h08; r_val[1]  <= dvo[15:0];           r_mask[1]  <= 16'hefff;
            r_addr[2]  <= 7'h09; r_val[2]  <= dvo[31:16];          r_mask[2]  <= 16'h7fff;
            r_addr[3]  <= 7'h07; r_val[3]  <= 16'h0000;            r_mask[3]  <= 16'h3c00;
            r_addr[4]  <= 7'h16; r_val[4]  <= 16'h1000;            r_mask[4]  <= 16'h3fff;  // DIVCLK = 1
            r_addr[5]  <= 7'h14; r_val[5]  <= dvm[15:0];           r_mask[5]  <= 16'hefff;
            r_addr[6]  <= 7'h15; r_val[6]  <= dvm[31:16];          r_mask[6]  <= 16'h7fff;
            r_addr[7]  <= 7'h13; r_val[7]  <= 16'h0000;            r_mask[7]  <= 16'h3c00;
            r_addr[8]  <= 7'h18; r_val[8]  <= {6'd0, lk[9:0]};     r_mask[8]  <= 16'h03ff;
            r_addr[9]  <= 7'h19; r_val[9]  <= {1'b0, lk[20:16], 10'h001}; r_mask[9]  <= 16'h7fff;
            r_addr[10] <= 7'h1a; r_val[10] <= {1'b0, lk[28:24], 10'h3e9}; r_mask[10] <= 16'h7fff;
            r_addr[11] <= 7'h4e; r_val[11] <= fl[31:16];           r_mask[11] <= 16'h9900;
            r_addr[12] <= 7'h4f; r_val[12] <= fl[15:0];            r_mask[12] <= 16'h9900;
            busy <= 1'b1; mmcm_rst <= 1'b1; idx <= 4'd0; wait_cnt <= 8'd0;
            st <= S_RST;
        end
        S_RST: begin                                   // hold reset a little before DRP access
            wait_cnt <= wait_cnt + 8'd1;
            if (wait_cnt == 8'd32) st <= S_RD;
        end
        S_RD: begin
            daddr <= r_addr[idx]; den <= 1'b1; st <= S_RDW;
        end
        S_RDW: if (drdy) begin
            di  <= (dout & ~r_mask[idx]) | (r_val[idx] & r_mask[idx]);
            st  <= S_WR;
        end
        S_WR: begin
            daddr <= r_addr[idx]; den <= 1'b1; dwe <= 1'b1; st <= S_WRW;
        end
        S_WRW: if (drdy) begin
            if (idx == NREG - 1) begin
                mmcm_rst <= 1'b0; lock_tmo <= 20'd0; st <= S_LOCK;
            end else begin
                idx <= idx + 4'd1; st <= S_RD;
            end
        end
        S_LOCK: begin                                  // wait for lock (timeout ~10 ms at 100 MHz)
            lock_tmo <= lock_tmo + 20'd1;
            if ((locked && lock_tmo > 20'd64) || lock_tmo == 20'hFFFFF) begin
                busy <= 1'b0; st <= S_IDLE;
            end
        end
        default: st <= S_IDLE;
    endcase
end

endmodule
`default_nettype wire
