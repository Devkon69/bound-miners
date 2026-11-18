// bound_sha256.v - una compressione SHA-256 iterativa (1 round per clock, 65 clock).
// BoundHash ne usa 8 per hash (seme, 4 per lo stato iniziale, 3 per il risultato): < 1 % del tempo.
`default_nettype none
module bound_sha256 (
    input  wire         clk,
    input  wire         start,      // impulso: carica st_in e blk
    input  wire [255:0] st_in,      // {a..h}, a nei bit alti
    input  wire [511:0] blk,        // 64 byte, il primo nei bit alti
    output reg          done,       // impulso: st_out valido
    output reg  [255:0] st_out
);
    function [31:0] kconst(input [5:0] i);
        case (i)
        0:kconst=32'h428a2f98; 1:kconst=32'h71374491; 2:kconst=32'hb5c0fbcf; 3:kconst=32'he9b5dba5;
        4:kconst=32'h3956c25b; 5:kconst=32'h59f111f1; 6:kconst=32'h923f82a4; 7:kconst=32'hab1c5ed5;
        8:kconst=32'hd807aa98; 9:kconst=32'h12835b01; 10:kconst=32'h243185be; 11:kconst=32'h550c7dc3;
        12:kconst=32'h72be5d74; 13:kconst=32'h80deb1fe; 14:kconst=32'h9bdc06a7; 15:kconst=32'hc19bf174;
        16:kconst=32'he49b69c1; 17:kconst=32'hefbe4786; 18:kconst=32'h0fc19dc6; 19:kconst=32'h240ca1cc;
        20:kconst=32'h2de92c6f; 21:kconst=32'h4a7484aa; 22:kconst=32'h5cb0a9dc; 23:kconst=32'h76f988da;
        24:kconst=32'h983e5152; 25:kconst=32'ha831c66d; 26:kconst=32'hb00327c8; 27:kconst=32'hbf597fc7;
        28:kconst=32'hc6e00bf3; 29:kconst=32'hd5a79147; 30:kconst=32'h06ca6351; 31:kconst=32'h14292967;
        32:kconst=32'h27b70a85; 33:kconst=32'h2e1b2138; 34:kconst=32'h4d2c6dfc; 35:kconst=32'h53380d13;
        36:kconst=32'h650a7354; 37:kconst=32'h766a0abb; 38:kconst=32'h81c2c92e; 39:kconst=32'h92722c85;
        40:kconst=32'ha2bfe8a1; 41:kconst=32'ha81a664b; 42:kconst=32'hc24b8b70; 43:kconst=32'hc76c51a3;
        44:kconst=32'hd192e819; 45:kconst=32'hd6990624; 46:kconst=32'hf40e3585; 47:kconst=32'h106aa070;
        48:kconst=32'h19a4c116; 49:kconst=32'h1e376c08; 50:kconst=32'h2748774c; 51:kconst=32'h34b0bcb5;
        52:kconst=32'h391c0cb3; 53:kconst=32'h4ed8aa4a; 54:kconst=32'h5b9cca4f; 55:kconst=32'h682e6ff3;
        56:kconst=32'h748f82ee; 57:kconst=32'h78a5636f; 58:kconst=32'h84c87814; 59:kconst=32'h8cc70208;
        60:kconst=32'h90befffa; 61:kconst=32'ha4506ceb; 62:kconst=32'hbef9a3f7; default:kconst=32'hc67178f2;
        endcase
    endfunction
    function [31:0] rr(input [31:0] x, input integer n); rr = (x >> n) | (x << (32 - n)); endfunction

    reg [255:0] base;
    reg [31:0]  a, b, c, d, e, f, g, h;
    reg [511:0] w;                   // finestra di 16 parole, w[511:480] = W[t]
    reg [6:0]   t;
    reg         run;

    wire [31:0] wt  = w[511:480];
    wire [31:0] w1  = w[479:448];    // W[t-15] dopo lo shift = parola 1
    wire [31:0] w9  = w[223:192];    // W[t-7]
    wire [31:0] w14 = w[63:32];      // W[t-2]
    wire [31:0] s0  = rr(w1, 7) ^ rr(w1, 18) ^ (w1 >> 3);
    wire [31:0] s1  = rr(w14, 17) ^ rr(w14, 19) ^ (w14 >> 10);
    wire [31:0] wnew = wt + s0 + w9 + s1;
    wire [31:0] t1 = h + (rr(e, 6) ^ rr(e, 11) ^ rr(e, 25)) + ((e & f) ^ (~e & g)) + kconst(t[5:0]) + wt;
    wire [31:0] t2 = (rr(a, 2) ^ rr(a, 13) ^ rr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));

    always @(posedge clk) begin
        done <= 1'b0;
        if (start) begin
            base <= st_in;
            {a, b, c, d, e, f, g, h} <= st_in;
            w <= blk; t <= 7'd0; run <= 1'b1;
        end else if (run) begin
            if (t == 7'd64) begin
                st_out <= {base[255:224] + a, base[223:192] + b, base[191:160] + c, base[159:128] + d,
                           base[127:96] + e,  base[95:64] + f,   base[63:32] + g,   base[31:0] + h};
                done <= 1'b1; run <= 1'b0;
            end else begin
                {a, b, c, d, e, f, g, h} <= {t1 + t2, a, b, c, d + t1, e, f, g};
                w <= {w[479:0], wnew};
                t <= t + 7'd1;
            end
        end
    end
    initial begin run = 1'b0; done = 1'b0; end
endmodule
`default_nettype wire
