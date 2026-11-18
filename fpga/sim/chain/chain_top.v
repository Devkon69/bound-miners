// chain_top.v - simulation wrapper: N BOUND FPGAs wired like the F2 hash board (from QTC).
`default_nettype none
module chain_top #(
    parameter integer N            = 3,
    parameter integer NCORE        = 1,
    parameter integer AW           = 6,
    parameter integer RDLAT        = 1,
    parameter integer BM2          = 0,
    parameter integer MULLAT       = 2,
    parameter integer CLKS_PER_BIT = 8,
    parameter integer CTL_HZ       = 1000000     // shortened "millisecond" for simulation
) (
    input  wire          clk_ctl,
    input  wire          clk_hash,
    input  wire          locked,
    input  wire          host_tx,
    output wire          host_rx,
    input  wire [12*N-1:0] temps,
    output wire [N-1:0]  hashing
);
    wire [N:0] cmd;
    wire [N:0] rsp;
    wire [N:0] b;
    assign cmd[0] = host_tx;
    assign host_rx = rsp[0];
    assign rsp[N] = 1'b1;
    assign b[0] = 1'b0;
    genvar i;
    generate for (i = 0; i < N; i = i + 1) begin : g
        wire [6:0] da; wire [15:0] ddi, ddo; wire den, dwe, drdy, mrst, lock_h;
        bound_fpga #(.NCORE(NCORE), .AW(AW), .RDLAT(RDLAT), .BM2(BM2), .MULLAT(MULLAT), .HASH_MHZ(95), .CTL_HZ(CTL_HZ), .CLKS_PER_BIT(CLKS_PER_BIT)) u (
            .clk_ctl(clk_ctl), .clk_hash(clk_hash), .locked_c(locked), .locked_h(lock_h & locked),
            .drp_addr(da), .drp_di(ddi), .drp_do(ddo), .drp_en(den), .drp_we(dwe), .drp_rdy(drdy),
            .mmcm_h_rst(mrst),
            .ci(cmd[i]), .co(cmd[i+1]), .ri(rsp[i+1]), .ro(rsp[i]),
            .bi(b[i]), .bo(b[i+1]), .addr_pins(i[2:0]), .led(),
            .sim_temp_code(temps[12*i +: 12]), .hashing(hashing[i]));
        sim_mmcm_drp #(.ID(i)) m (.clk(clk_ctl), .daddr(da), .di(ddi), .dout(ddo), .den(den), .dwe(dwe),
                                  .drdy(drdy), .rst(mrst), .locked(lock_h));
    end endgenerate
endmodule
// behavioural MMCM DRP: registers start at 0xFFFF, DRDY two cycles after DEN,
// LOCKED low during RST and for 50 cycles after it; every write is printed.
module sim_mmcm_drp #(parameter integer ID = 0) (
    input  wire clk, input wire [6:0] daddr, input wire [15:0] di, output reg [15:0] dout,
    input  wire den, input wire dwe, output reg drdy, input wire rst, output wire locked
);
    reg [15:0] regs [0:127];
    reg [1:0] dl; reg pend_we; reg [6:0] pa; reg [15:0] pd;
    reg [7:0] lk = 8'd0;
    integer k;
    initial for (k = 0; k < 128; k = k + 1) regs[k] = 16'hFFFF;
    initial begin dl = 0; drdy = 0; end
    assign locked = (lk == 8'd50);
    always @(posedge clk) begin
        drdy <= 1'b0;
        if (rst) lk <= 8'd0; else if (lk != 8'd50) lk <= lk + 8'd1;
        if (den) begin dl <= 2'd2; pend_we <= dwe; pa <= daddr; pd <= di; end
        else if (dl != 0) begin
            dl <= dl - 2'd1;
            if (dl == 2'd1) begin
                drdy <= 1'b1;
                if (pend_we) begin
                    regs[pa] <= pd;
                    if (!rst) $display("DRP_ERR %0d write without reset", ID);
                    $display("DRP_W %0d %02x %04x", ID, pa, pd);
                end else dout <= regs[pa];
            end
        end
    end
endmodule
`default_nettype wire
