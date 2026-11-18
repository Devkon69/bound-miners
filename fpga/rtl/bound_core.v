// bound_core.v - un core BoundHash (proof of work della moneta BOUND, github.com/Sonic-Bound/Bound).
//
// Un hash alla volta: l'algoritmo e' una catena di 2 x LINES passi, ognuno dipende dal precedente
// e legge / scrive lo scratchpad a un indirizzo deciso dai dati, quindi non si srotola in pipeline.
// Si guadagna mettendo piu' core: ognuno vuole il suo scratchpad da 512 KiB (bound_ram).
//
//   1. seme   = SHA-256 dell'header (l'host manda lo stato dopo i primi 64 byte: `mid`)
//      S      = 4 x SHA-256(seme || k)                                  (16 parole da 64 bit)
//   2. fill   M[i] = F(M[i-1] ^ M[ref], i)        ref scelto da M[i-1]  (LINES passi)
//   3. mix    v ^= M[j]; moltiplicazione 64x64; round; M[j] += v        (LINES passi)
//   4. hash   = SHA-256 di seme e v
//
// Il round e' quello di BLAKE2b con il BlaMka di Argon2 (x + y + 2*lo32(x)*lo32(y)): 8 quarti di G
// uno dopo l'altro, 4 G in parallelo -> 4 moltiplicatori 32x32 condivisi, 8 clock per round (BM2=0)
// o 16 (BM2=1: prodotto registrato, poi somma: per salire di clock).
//
// Clock per passo (RDLAT 1, MULLAT 2, BM2 0): fill 10, mix 12 -> ~90.700 clock per hash a 4096 righe.
`default_nettype none
module bound_core #(
    parameter integer AW        = 12,        // log2 delle righe: 12 = mainnet e testnet (512 KiB)
    parameter         RAM_STYLE = "block",   // "block" o "ultra"
    parameter integer RDLAT     = 1,         // latenza di lettura della RAM (1 o 2)
    parameter integer BM2       = 0,         // 1 = BlaMka in due clock
    parameter integer MULLAT    = 2          // registri sul prodotto 64x64 (>= 1)
) (
    input  wire         clk,
    input  wire         rst,
    input  wire         start,      // impulso con il core fermo (busy = 0)
    input  wire [255:0] mid,        // stato SHA-256 dopo tag||tag e i primi 64 byte dell'header
    input  wire [95:0]  tail,       // byte 64..75 dell'header, il 64 nei bit alti
    input  wire [31:0]  nonce,      // byte 76..79 = nonce little-endian
    output reg          busy,
    output reg          done,       // impulso
    output reg  [255:0] hash,       // come numero: da confrontare con il target (hash <= target)
    output reg  [31:0]  hash_nonce
);
`include "bound_const.vh"
    localparam [255:0] SHA_IV = 256'h6a09e667bb67ae853c6ef372a54ff53a510e527f9b05688c1f83d9ab5be0cd19;
    localparam [AW-1:0] LAST = {AW{1'b1}};

    function [63:0] bswap64(input [63:0] x);
        bswap64 = {x[7:0], x[15:8], x[23:16], x[31:24], x[39:32], x[47:40], x[55:48], x[63:56]};
    endfunction
    function [63:0] rotr64(input [63:0] x, input [1:0] sel);   // 0:32 1:24 2:16 3:63
        case (sel)
            2'd0: rotr64 = {x[31:0], x[63:32]};
            2'd1: rotr64 = {x[23:0], x[63:24]};
            2'd2: rotr64 = {x[15:0], x[63:16]};
            default: rotr64 = {x[62:0], x[63]};
        endcase
    endfunction

    // ------------------------------------------------------------------ stato
    localparam [3:0] S_IDLE = 0, S_SHA = 1, S_SHAW = 2, S_F0 = 3, S_ROUND = 4, S_FFIN = 5, S_FLOAD = 6,
                     S_MINIT = 7, S_MRD0 = 8, S_MXOR = 9, S_MMUL = 10, S_MAP = 11, S_WAIT = 12;
    reg [3:0]  state, after_wait;
    reg [1:0]  wcnt;
    reg [2:0]  sidx;                 // quale SHA-256 (0 seme, 1..4 stato iniziale, 5..7 risultato)
    reg [255:0] seed, hst;
    reg [1023:0] S;                  // stato iniziale, parola k in [64k +: 64]
    reg [63:0] A [0:3];              // v0..v3
    reg [63:0] B [0:3];              // v4..v7
    reg [63:0] C [0:3];              // v8..v11
    reg [63:0] D [0:3];              // v12..v15
    reg [1023:0] X;                  // fill: ingresso del round, poi la riga; mix: la riga letta
    reg [AW-1:0] idx;                // riga (fill) o passo (mix)
    reg        mixing, first;
    reg [2:0]  q;                    // quarto di G: 0..3 colonne, 4..7 diagonali
    reg        ph;
    reg [AW-1:0] ref_r, jn, jo;
    reg [2:0]  mcnt;
    reg [95:0] tail_r;

    wire [1023:0] V = {D[3], D[2], D[1], D[0], C[3], C[2], C[1], C[0],
                       B[3], B[2], B[1], B[0], A[3], A[2], A[1], A[0]};

    // ------------------------------------------------------------------ SHA-256
    reg          sha_start;
    reg  [255:0] sha_st;
    reg  [511:0] sha_blk;
    wire         sha_done;
    wire [255:0] sha_out;
    bound_sha256 u_sha (.clk(clk), .start(sha_start), .st_in(sha_st), .blk(sha_blk),
                        .done(sha_done), .st_out(sha_out));

    wire [1023:0] vstream;           // v come 128 byte (parole little-endian), il primo nei bit alti
    genvar gi;
    generate for (gi = 0; gi < 16; gi = gi + 1) begin : g_vs
        assign vstream[1023 - 64*gi -: 64] = bswap64(V[64*gi +: 64]);
    end endgenerate

    always @* begin
        case (sidx)
            3'd0: begin sha_st = mid;    sha_blk = {tail_r, hash_nonce[7:0], hash_nonce[15:8], hash_nonce[23:16],
                                                    hash_nonce[31:24], 8'h80, 312'd0, 64'd1152}; end
            3'd5: begin sha_st = MID_RES; sha_blk = {seed, vstream[1023:768]}; end
            3'd6: begin sha_st = hst;    sha_blk = vstream[767:256]; end
            3'd7: begin sha_st = hst;    sha_blk = {vstream[255:0], 8'h80, 184'd0, 64'd1792}; end
            default: begin sha_st = SHA_IV; sha_blk = {seed, 5'd0, sidx - 3'd1, 8'h80, 176'd0, 64'd264}; end
        endcase
    end

    // ------------------------------------------------------------------ scratchpad
    reg           ram_we;
    reg  [AW-1:0] ram_wa, ram_ra;
    reg  [1023:0] ram_wd;
    wire [1023:0] ram_rd;
    bound_ram #(.AW(AW), .STYLE(RAM_STYLE), .RDLAT(RDLAT)) u_ram (
        .clk(clk), .we(ram_we), .waddr(ram_wa), .wdata(ram_wd), .raddr(ram_ra), .rdata(ram_rd));

    // ------------------------------------------------------------------ round: 4 BlaMka condivisi
    wire        qodd  = q[0];                        // pari: a = BM(a,b), d ^= a; dispari: c = BM(c,d), b ^= c
    wire [1:0]  rsel  = q[1:0];                      // rotazione 32, 24, 16, 63
    reg  [63:0] pr [0:3];
    wire [63:0] bx [0:3], by [0:3], pc [0:3], psel [0:3], bm [0:3], xr [0:3];
    generate for (gi = 0; gi < 4; gi = gi + 1) begin : g_bm
        assign bx[gi]   = qodd ? C[gi] : A[gi];
        assign by[gi]   = qodd ? D[gi] : B[gi];
        assign pc[gi]   = bx[gi][31:0] * by[gi][31:0];
        assign psel[gi] = BM2 ? pr[gi] : pc[gi];
        assign bm[gi]   = bx[gi] + by[gi] + {psel[gi][62:0], 1'b0};
        assign xr[gi]   = rotr64((qodd ? B[gi] : D[gi]) ^ bm[gi], rsel);
    end endgenerate
    wire qlast = (BM2 == 0) || ph;                   // ultimo clock di questo quarto
    wire rlast = (q == 3'd7) && qlast;               // ultimo clock del round

    // fill: riga a cui punta la prossima (ref), dal valore finale di v0 ^ x0
    wire [31:0]    out0  = A[0][31:0] ^ X[31:0];
    wire [AW+31:0] refp  = out0 * idx;               // idx = (i+1) - 1
    // mix
    wire [1023:0] wsum;
    generate for (gi = 0; gi < 16; gi = gi + 1) begin : g_ws
        assign wsum[64*gi +: 64] = X[64*gi +: 64] + V[64*gi +: 64];
    end endgenerate
    wire          bypass = !first && (jn == jo);     // la riga da leggere e' quella che sto scrivendo
    wire [1023:0] mline  = bypass ? wsum : ram_rd;
    reg  [127:0]  mp [0:3];
    wire [127:0]  mprod = A[0] * (A[1] | 64'd1);
    wire [127:0]  mres  = mp[MULLAT-1];
    // fill
    wire [1023:0] fout  = V ^ X;
    wire [AW:0]   idxn  = {1'b0, idx} + 1'b1;
    wire [1023:0] fline = (idx == {AW{1'b0}}) ? 1024'd0 : ram_rd;   // la riga 1 non ha ref
    wire [1023:0] fx    = X ^ fline;

    integer k;
    task load_v(input [1023:0] d, input [63:0] tweak);
        begin
            for (k = 0; k < 4; k = k + 1) begin
                A[k] <= d[64*k +: 64];
                B[k] <= d[64*(4+k) +: 64];
                C[k] <= d[64*(8+k) +: 64];
                D[k] <= d[64*(12+k) +: 64] ^ ((k == 3) ? tweak : 64'd0);
            end
        end
    endtask

    always @* begin
        ram_we = 1'b0; ram_wa = idx; ram_wd = fout; ram_ra = A[0][AW-1:0];
        if (state == S_FFIN) begin ram_we = 1'b1; ram_ra = ref_r; end
        if (state == S_MXOR) begin ram_we = !first; ram_wa = jo; ram_wd = wsum; end
    end

    always @(posedge clk) begin
        done <= 1'b0; sha_start <= 1'b0;
        if (BM2) begin for (k = 0; k < 4; k = k + 1) pr[k] <= pc[k]; end
        mp[0] <= mprod; mp[1] <= mp[0]; mp[2] <= mp[1]; mp[3] <= mp[2];

        case (state)
        S_IDLE: if (start) begin
            busy <= 1'b1; hash_nonce <= nonce; tail_r <= tail;
            sidx <= 3'd0; state <= S_SHA;
        end
        S_SHA:  begin sha_start <= 1'b1; state <= S_SHAW; end
        S_SHAW: if (sha_done) begin
            case (sidx)
            3'd0: seed <= sha_out;
            3'd5, 3'd6: hst <= sha_out;
            3'd7: begin
                for (k = 0; k < 32; k = k + 1) hash[8*k +: 8] <= sha_out[255 - 8*k -: 8];
                done <= 1'b1; busy <= 1'b0;
            end
            default: for (k = 0; k < 4; k = k + 1)
                S[64*(4*(sidx-1)+k) +: 64] <= bswap64(sha_out[255 - 64*k -: 64]);
            endcase
            sidx  <= sidx + 3'd1;
            state <= (sidx == 3'd7) ? S_IDLE : (sidx == 3'd4) ? S_F0 : S_SHA;
        end

        // ---------------- fill
        S_F0: begin                                   // riga 0 = F(S, 0)
            X <= S; load_v(S, 64'd0);
            idx <= {AW{1'b0}}; mixing <= 1'b0; q <= 3'd0; ph <= 1'b0; state <= S_ROUND;
        end
        S_FFIN: begin                                 // scrive M[idx] = v ^ x, legge M[ref]
            X <= fout;
            if (idx == LAST) state <= S_MINIT;
            else if (RDLAT > 1) begin wcnt <= RDLAT - 2; after_wait <= S_FLOAD; state <= S_WAIT; end
            else state <= S_FLOAD;
        end
        S_FLOAD: begin                                // x = M[idx] ^ M[ref], round con la riga idx+1
            X <= fx; load_v(fx, {{(63-AW){1'b0}}, idxn});
            idx <= idxn[AW-1:0]; q <= 3'd0; ph <= 1'b0; state <= S_ROUND;
        end

        // ---------------- mix
        S_MINIT: begin                                // v = S ^ M[LAST]
            load_v(S ^ X, 64'd0);
            mixing <= 1'b1; first <= 1'b1; idx <= {AW{1'b0}}; state <= S_MRD0;
        end
        S_MRD0: begin                                 // legge M[v0 mod LINES]
            jn <= A[0][AW-1:0];
            if (RDLAT > 1) begin wcnt <= RDLAT - 2; after_wait <= S_MXOR; state <= S_WAIT; end
            else state <= S_MXOR;
        end
        S_MXOR: begin                                 // scrive la riga del passo prima, v ^= riga nuova
            load_v(V ^ mline, 64'd0);
            X <= mline; jo <= jn; first <= 1'b0;
            mcnt <= 3'd0; state <= S_MMUL;
        end
        S_MMUL: begin                                 // v0 * (v1 | 1) attraverso MULLAT registri
            mcnt <= mcnt + 3'd1;
            if (mcnt == MULLAT - 1) state <= S_MAP;
        end
        S_MAP: begin
            A[2] <= A[2] + mres[127:64];
            A[3] <= A[3] ^ mres[63:0];
            D[3] <= D[3] ^ {{(64-AW){1'b0}}, idx};
            q <= 3'd0; ph <= 1'b0; state <= S_ROUND;
        end

        S_WAIT: begin
            wcnt <= wcnt - 2'd1;
            if (wcnt == 2'd0) state <= after_wait;
        end

        // ---------------- round (fill e mix)
        S_ROUND: begin
            if (BM2 && !ph) ph <= 1'b1;
            else begin
                ph <= 1'b0; q <= q + 3'd1;
                if (!qodd) begin
                    for (k = 0; k < 4; k = k + 1) begin A[k] <= bm[k]; D[k] <= xr[k]; end
                end else if (q[1:0] != 2'd3) begin
                    for (k = 0; k < 4; k = k + 1) begin C[k] <= bm[k]; B[k] <= xr[k]; end
                end else if (!q[2]) begin             // fine delle colonne: le diagonali diventano colonne
                    for (k = 0; k < 4; k = k + 1) begin
                        B[k] <= xr[(k + 1) & 3]; C[k] <= bm[(k + 2) & 3]; D[k] <= D[(k + 3) & 3];
                    end
                end else begin                        // fine delle diagonali: si torna alle colonne
                    for (k = 0; k < 4; k = k + 1) begin
                        B[k] <= xr[(k + 3) & 3]; C[k] <= bm[(k + 2) & 3]; D[k] <= D[(k + 1) & 3];
                    end
                end
                if (rlast) begin
                    if (!mixing) begin
                        ref_r <= refp[AW+31:32];
                        state <= S_FFIN;
                    end else if (idx == LAST) begin
                        sidx <= 3'd5; state <= S_SHA;       // v finale -> risultato
                    end else begin
                        idx <= idx + 1'b1;
                        jn  <= A[0][AW-1:0];                 // v0 e' gia' quello finale (ram_ra = v0)
                        if (RDLAT > 1) begin wcnt <= RDLAT - 2; after_wait <= S_MXOR; state <= S_WAIT; end
                        else state <= S_MXOR;
                    end
                end
            end
        end
        default: state <= S_IDLE;
        endcase

        if (rst) begin state <= S_IDLE; busy <= 1'b0; done <= 1'b0; end
    end
    initial begin state = S_IDLE; busy = 1'b0; done = 1'b0; end
endmodule
`default_nettype wire
