// xadc_temp.v - reads the 7-series XADC die temperature once per millisecond.
// code (12 bit): T[degC] = code * 503.975 / 4096 - 273.15
// The XADC's own over-temperature alarm (125 degC, automatic power-down of the
// configuration logic is NOT enabled) stays at its default.
`default_nettype none
module xadc_temp #(
    parameter integer CTL_HZ = 100000000
) (
    input  wire        clk,         // DRP clock (control domain, 100 MHz)
    input  wire        rst,
`ifdef SIM
    input  wire [11:0] sim_code,
`endif
    output reg  [11:0] code,
    output reg         stb
);
    localparam integer PER = CTL_HZ / 1000;
    reg [$clog2(PER+1)-1:0] cnt;
    reg        den;
    wire       drdy;
    wire [15:0] dout;

`ifdef SIM
    reg [3:0] lat;
    assign drdy = (lat == 4'd1);
    assign dout = {sim_code, 4'h0};
    always @(posedge clk) lat <= den ? 4'd5 : (lat != 0 ? lat - 4'd1 : 4'd0);
`else
    XADC #(
        .INIT_40(16'h0000),          // channel 0 = temperature, no averaging
        .INIT_41(16'h3000),          // single channel continuous, alarms enabled
        .INIT_42(16'h0400),          // ADCCLK = DCLK / 4 = 25 MHz
        .SIM_DEVICE("7SERIES")
    ) u_xadc (
        .DCLK(clk), .RESET(rst), .DEN(den), .DWE(1'b0), .DADDR(7'h00), .DI(16'h0000),
        .DO(dout), .DRDY(drdy),
        .CONVST(1'b0), .CONVSTCLK(1'b0), .VP(1'b0), .VN(1'b0),
        .VAUXP(16'h0000), .VAUXN(16'h0000),
        .ALM(), .OT(), .CHANNEL(), .EOC(), .EOS(), .BUSY(),
        .JTAGBUSY(), .JTAGLOCKED(), .JTAGMODIFIED(), .MUXADDR()
    );
`endif

    always @(posedge clk) begin
        den <= 1'b0; stb <= 1'b0;
        if (rst) begin
            cnt <= 0; code <= 12'd0;
        end else begin
            if (cnt == PER - 1) begin cnt <= 0; den <= 1'b1; end
            else cnt <= cnt + 1'b1;
            if (drdy) begin code <= dout[15:4]; stb <= 1'b1; end
        end
    end
endmodule
`default_nettype wire
