// bound_engine.v - motore BoundHash (moneta BOUND): NCORE core sullo stesso lavoro, dominio del clock hash.
// Stesse porte di prox_engine / rigor_engine: interfaccia di catena e wrapper dell'FPGA non cambiano.
//
// JOB (pacchetto da 209 byte come gli altri bitstream, vedi docs/PROTOCOLLO.md):
//   job_target  64 bit alti del target: candidato se hash[255:192] <= job_target
//   job_state   byte 0..31   mid: stato SHA-256 (8 parole big-endian) dopo tag||tag e i primi 64 byte dell'header
//               byte 32..43  byte 64..75 dell'header (fine del merkle root, time, bits)
//               byte 44..47  base del nonce (little-endian), di solito 0
//               il resto a zero
// Nonce del core c nell'FPGA in posizione pos: base + {c (IDW bit), pos (4 bit), contatore}.
// Un lavoro mandato a tutti (ADDR 0xFF) da' quindi nonce diversi a ogni FPGA della catena.
// Con ~1 kH/s per core il contatore (25 bit con 8 core) dura ore: il lavoro non si esaurisce mai,
// st_job_valid resta alto finche' non arriva un lavoro nuovo o job_clear.
// Lavoro nuovo: ogni core finisce l'hash in corso (< 2 ms), lo scarta e riparte.
// I primi NULTRA core hanno lo scratchpad in URAM, gli altri in BRAM.
`default_nettype none
module bound_engine #(
    parameter integer NCORE  = 7,
    parameter integer NULTRA = 0,
    parameter integer IDW    = 3,           // bit dell'id del core nel nonce (2^IDW >= NCORE)
    parameter integer AW     = 12,          // log2 delle righe dello scratchpad (12 = mainnet)
    parameter integer RDLAT  = 1,
    parameter integer BM2    = 0,
    parameter integer MULLAT = 2
) (
    input  wire          clk,
    input  wire          rst,
    input  wire          en,                // 0: i core si fermano alla fine dell'hash
    input  wire [3:0]    pos,
    input  wire          job_load,
    input  wire [7:0]    job_id,
    input  wire [63:0]   job_target,
    input  wire [1599:0] job_state,
    input  wire          job_clear,
    output reg           cand_valid,
    input  wire          cand_ready,
    output reg  [79:0]   cand_data,         // {id, nonce[31:0], 6'b0, core[3:0], 6'b0, top24}
    output reg  [31:0]   st_iter,           // hash avviati dal core 0 in questo lavoro
    output reg  [7:0]    st_job_id,
    output reg           st_job_valid,
    output reg  [7:0]    st_drops,          // sempre 0: un core aspetta che il suo candidato esca
    output reg  [31:0]   st_batches         // hash finiti da tutti i core in questo lavoro
);
    localparam integer CW = 32 - IDW - 4;   // bit del contatore

    reg [255:0] mid_r; reg [95:0] tail_r; reg [31:0] base_r; reg [63:0] tgt_r;
    reg [7:0]   jid; reg [3:0] pos_r;
    reg         have_job;
    reg         epoch;                      // cambia a ogni lavoro: scarta gli hash del lavoro vecchio
    integer b;
    always @(posedge clk) begin
        if (job_load) begin
            for (b = 0; b < 32; b = b + 1) mid_r[255 - 8*b -: 8] <= job_state[8*b +: 8];
            for (b = 0; b < 12; b = b + 1) tail_r[95 - 8*b -: 8] <= job_state[8*(32+b) +: 8];
            base_r <= job_state[8*44 +: 32];
            tgt_r <= job_target; jid <= job_id; pos_r <= pos;
            have_job <= 1'b1; epoch <= ~epoch;
        end else if (job_clear) begin have_job <= 1'b0; epoch <= ~epoch; end
        if (rst) have_job <= 1'b0;
    end
    initial begin have_job = 1'b0; epoch = 1'b0; end

    wire [NCORE-1:0]     c_done, c_busy;
    wire [256*NCORE-1:0] c_hash;
    wire [32*NCORE-1:0]  c_nonce;
    reg  [NCORE-1:0]     c_start;
    reg  [NCORE-1:0]     c_epoch;           // lavoro con cui il core e' partito
    reg  [NCORE-1:0]     pend;              // candidato in attesa di uscire
    reg  [31:0]          p_nonce [0:NCORE-1];
    reg  [23:0]          p_top   [0:NCORE-1];
    reg  [CW-1:0]        cnt     [0:NCORE-1];

    genvar gi;
    generate for (gi = 0; gi < NCORE; gi = gi + 1) begin : g_core
        localparam [IDW-1:0] CID = gi;
        wire [31:0] nonce_c = base_r + {CID, pos_r, cnt[gi]};
        if (gi < NULTRA) begin : g_u
            bound_core #(.AW(AW), .RAM_STYLE("ultra"), .RDLAT(RDLAT), .BM2(BM2), .MULLAT(MULLAT)) u_core (
                .clk(clk), .rst(rst), .start(c_start[gi]), .mid(mid_r), .tail(tail_r), .nonce(nonce_c),
                .busy(c_busy[gi]), .done(c_done[gi]), .hash(c_hash[256*gi +: 256]), .hash_nonce(c_nonce[32*gi +: 32]));
        end else begin : g_b
            bound_core #(.AW(AW), .RAM_STYLE("block"), .RDLAT(RDLAT), .BM2(BM2), .MULLAT(MULLAT)) u_core (
                .clk(clk), .rst(rst), .start(c_start[gi]), .mid(mid_r), .tail(tail_r), .nonce(nonce_c),
                .busy(c_busy[gi]), .done(c_done[gi]), .hash(c_hash[256*gi +: 256]), .hash_nonce(c_nonce[32*gi +: 32]));
        end
    end endgenerate

    integer i;
    reg         found;
    reg [31:0]  hc;
    wire        newjob = job_load || job_clear;
    reg [NCORE-1:0] good;
    always @* begin
        for (i = 0; i < NCORE; i = i + 1)
            good[i] = c_done[i] && (c_epoch[i] == epoch) && !newjob && have_job
                      && (c_hash[256*i + 192 +: 64] <= tgt_r);
    end

    always @(posedge clk) begin
        c_start <= {NCORE{1'b0}};
        // piu' core possono finire nello stesso clock: conta quelli del lavoro in corso
        hc = st_batches;
        for (i = 0; i < NCORE; i = i + 1)
            if (c_done[i] && c_epoch[i] == epoch) hc = hc + 32'd1;
        st_batches <= newjob ? 32'd0 : hc;

        for (i = 0; i < NCORE; i = i + 1) begin
            if (newjob) begin cnt[i] <= {CW{1'b0}}; pend[i] <= 1'b0; end
            else begin
                if (good[i]) begin
                    pend[i] <= 1'b1; p_nonce[i] <= c_nonce[32*i +: 32]; p_top[i] <= c_hash[256*i + 232 +: 24];
                end
                // parte quando e' fermo, c'e' un lavoro e il candidato precedente e' uscito
                if (have_job && en && !c_busy[i] && !c_start[i] && !c_done[i] && !pend[i]) begin
                    c_start[i] <= 1'b1; c_epoch[i] <= epoch;
                end
                if (c_start[i]) cnt[i] <= cnt[i] + 1'b1;
            end
        end

        // un candidato alla volta, tenuto finche' cand_ready
        if (cand_valid && cand_ready) cand_valid <= 1'b0;
        if (newjob) cand_valid <= 1'b0;
        else if (!cand_valid || cand_ready) begin
            found = 1'b0;
            for (i = 0; i < NCORE; i = i + 1)
                if (!found && pend[i]) begin
                    found = 1'b1; pend[i] <= 1'b0; cand_valid <= 1'b1;
                    cand_data <= {jid, p_nonce[i], 6'd0, i[3:0], 6'd0, p_top[i]};
                end
        end

        st_iter      <= {{(32-CW){1'b0}}, cnt[0]};
        st_job_id    <= jid;
        st_job_valid <= have_job;
        st_drops     <= 8'd0;
        if (rst) begin
            pend <= {NCORE{1'b0}}; c_start <= {NCORE{1'b0}}; cand_valid <= 1'b0; st_batches <= 32'd0;
        end
    end
    initial begin pend = 0; c_start = 0; cand_valid = 0; st_batches = 0; end
endmodule
`default_nettype wire
