// uart.v - 8N1 UART receiver and transmitter, fixed baud (CLKS_PER_BIT).
`default_nettype none

module uart_rx #(
    parameter integer CLKS_PER_BIT = 868
) (
    input  wire       clk,
    input  wire       rst,
    input  wire       rx,
    output reg        valid,
    output reg  [7:0] data
);
    (* ASYNC_REG = "TRUE" *) reg [2:0] sync;
    always @(posedge clk) sync <= {sync[1:0], rx};
    wire s = sync[2];

    localparam integer CW = $clog2(CLKS_PER_BIT * 3 / 2 + 2);
    reg [CW-1:0] cnt;
    reg [3:0]    bitn;
    reg          busy;
    reg [7:0]    sh;

    always @(posedge clk) begin
        valid <= 1'b0;
        if (rst) begin
            busy <= 1'b0; cnt <= 0; bitn <= 4'd0;
        end else if (!busy) begin
            if (!s) begin                        // start bit edge
                busy <= 1'b1;
                cnt  <= CLKS_PER_BIT + CLKS_PER_BIT / 2 - 1;  // middle of bit 0
                bitn <= 4'd0;
            end
        end else if (cnt != 0) begin
            cnt <= cnt - 1'b1;
        end else if (bitn < 4'd8) begin
            sh   <= {s, sh[7:1]};
            bitn <= bitn + 4'd1;
            cnt  <= CLKS_PER_BIT - 1;
        end else begin                           // stop bit sample
            busy <= 1'b0;
            if (s) begin valid <= 1'b1; data <= sh; end
        end
    end
endmodule

module uart_tx #(
    parameter integer CLKS_PER_BIT = 868
) (
    input  wire       clk,
    input  wire       rst,
    input  wire       valid,
    input  wire [7:0] data,
    output wire       ready,
    output reg        tx
);
    localparam integer CW = $clog2(CLKS_PER_BIT + 1);
    reg [CW-1:0] cnt;
    reg [3:0]    bitn;
    reg [9:0]    sh;
    reg          busy;
    assign ready = !busy;

    always @(posedge clk) begin
        if (rst) begin
            busy <= 1'b0; tx <= 1'b1; cnt <= 0; bitn <= 4'd0;
        end else if (!busy) begin
            tx <= 1'b1;
            if (valid) begin
                sh   <= {1'b1, data, 1'b0};
                busy <= 1'b1; bitn <= 4'd0; cnt <= 0;
            end
        end else if (cnt != 0) begin
            cnt <= cnt - 1'b1;
        end else if (bitn == 4'd10) begin
            busy <= 1'b0;
        end else begin
            tx   <= sh[0];
            sh   <= {1'b1, sh[9:1]};
            bitn <= bitn + 4'd1;
            cnt  <= CLKS_PER_BIT - 1;
        end
    end
endmodule

// small synchronous FIFO (distributed RAM)
module sfifo #(
    parameter integer W = 8,
    parameter integer DEPTH_LOG2 = 5
) (
    input  wire         clk,
    input  wire         rst,
    input  wire         wr,
    input  wire [W-1:0] din,
    input  wire         rd,
    output wire [W-1:0] dout,
    output wire         empty,
    output wire         full,
    output wire [DEPTH_LOG2:0] count
);
    reg [W-1:0] mem [0:(1<<DEPTH_LOG2)-1];
    reg [DEPTH_LOG2:0] wp, rp;
    assign count = wp - rp;
    assign empty = (wp == rp);
    assign full  = (count == (1 << DEPTH_LOG2));
    assign dout  = mem[rp[DEPTH_LOG2-1:0]];
    always @(posedge clk) begin
        if (rst) begin
            wp <= 0; rp <= 0;
        end else begin
            if (wr && !full) begin
                mem[wp[DEPTH_LOG2-1:0]] <= din;
                wp <= wp + 1'b1;
            end
            if (rd && !empty) rp <= rp + 1'b1;
        end
    end
endmodule

`default_nettype wire
