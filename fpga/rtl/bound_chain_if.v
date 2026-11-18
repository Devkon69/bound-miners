// bound_chain_if.v - da prox_chain_if.v (proxima-f2, 13/11/2026): cambia solo la sigla di INFO ("BND1").
// Il contenuto del JOB (209 byte) per BOUND e' descritto in bound_engine.v e docs/PROTOCOLLO.md.
// prox_chain_if.v - (from bc3_chain_if.v / qtc_chain_if.v) serial chain interface of one FPGA on the
// BlackMiner F2 hash board (control clock domain). Derived from NOID v1.1 (same framing, thermal,
// watchdog and CLK command); JOB, CAND and INFO carry the PROXIMA fields.
//
// Topology (schematic P0028, page 10): CPU_TXD -> FPGA1.CI, FPGAn.CO -> FPGAn+1.CI
// (commands flow down); FPGAn+1.RO -> FPGAn.RI, FPGA1.RO -> CPU_RXD (responses
// flow up). Every FPGA re-transmits each command byte downstream and merges its
// own responses with the ones coming from below.
//
// Command  (host -> FPGAs):  A5 5A CMD ADDR LEN payload[LEN] CRC16(hi) CRC16(lo)
//   CRC16-CCITT (poly 0x1021, init 0xFFFF) over CMD, ADDR, LEN, payload.
//   ADDR 0xFF = broadcast, otherwise the chain position.
//   0x01 ENUM   LEN 2  [pos, ~pos]  each FPGA takes pos and forwards pos+1 (CRC ignored)
//   0x02 JOB    LEN 209 id, target64 (8 bytes big-endian: target >> 192, top 64 bits of the LE 256-bit target),
//                       state (200 bytes = 25 Keccak lanes little-endian: padded preimage with the nonce at zero)
//   0x03 CTRL   LEN 4  flags(b0 hash enable, b1 clear job), trip degC, resume degC, watchdog s
//   0x04 STATUS LEN 0
//   0x05 INFO   LEN 0
// Response (FPGA -> host), 16 bytes: 5A A5 TYPE POS payload[10] CRC16(hi) CRC16(lo)
//   CRC over TYPE, POS, payload.
//   0x81 CAND   id, nonce32 (low nonce), idx16 (core<<6), top24 (top 24 bits of lane 3 = digest bytes 31,30,29)
//   0x82 STATUS temp12(2), flags, addr straps, job id, iter32, drops
//   0x83 INFO   'R','G','R','1', ver major, ver minor, cores, 24 (rounds),
//               current MHz[7:0], {built MHz / 5 [6:0], current MHz[8]}   (v1.1)
//   0x06 CLK    M, O, MHz lo, MHz hi : hash clock = 25 MHz * M / O (MMCM DRP)
`default_nettype none
(* use_dsp = "no" *)
module bound_chain_if #(
    parameter integer CLKS_PER_BIT = 868,     // 100 MHz / 115200
    parameter integer CTL_HZ       = 100000000,
    parameter integer LANES        = 8,
    parameter integer HASH_MHZ     = 250,
    parameter integer CONTEXTS     = 64,
    parameter [7:0]   VER_MAJOR    = 8'd1,
    parameter [7:0]   VER_MINOR    = 8'd0,
    parameter integer MAX_MHZ      = 400      // absolute ceiling accepted by C_CLK
) (
    input  wire         clk,
    input  wire         rst,
    // serial chain
    input  wire         ci,
    output wire         co,
    input  wire         ri,
    output wire         ro,
    // board pins
    input  wire         bi,
    input  wire [2:0]   addr_pins,
    // temperature (XADC code, 12 bit) and valid strobe
    input  wire [11:0]  temp_code,
    input  wire         temp_stb,
    // to hash domain
    output reg          hash_en,              // level, synchronise in the hash domain
    output reg          job_tgl,              // toggles when a job event is staged
    input  wire         job_ack_tgl,          // from hash domain (unsynchronised)
    output reg          job_kind,             // 1 = load, 0 = clear
    output reg  [3:0]   job_pos,
    output reg  [7:0]   job_id,
    output reg  [63:0]  job_target,           // first 8 bytes of the target as a big-endian number
    output reg  [1599:0] job_state,           // Keccak state, lane i = bits [64*i +: 64]
    // candidates from hash domain (toggle handshake, data stable while pending)
    input  wire         cand_req_tgl,
    input  wire [79:0]  cand_data,
    output reg          cand_ack_tgl,
    // status snapshot from hash domain
    output reg          snap_req_tgl,
    input  wire         snap_ack_tgl,
    input  wire [31:0]  snap_iter,
    input  wire [7:0]   snap_job_id,
    input  wire         snap_job_valid,
    input  wire [7:0]   snap_drops,
    // misc
    output reg          thermal_trip,
    output reg          enumerated,
    output reg  [3:0]   my_pos,
    // hash clock reprogramming
    output reg          clk_start,            // one-cycle request to mmcm_drp
    output reg  [5:0]   clk_m,
    output reg  [6:0]   clk_o,
    input  wire         clk_busy,
    input  wire         hash_ok               // hash domain running (synchronised, settled)
);

// ======================================================================= helpers
function [15:0] crc16_byte;
    input [15:0] c;
    input [7:0]  b;
    integer n;
    reg [15:0] x;
    begin
        x = c ^ {b, 8'h00};
        for (n = 0; n < 8; n = n + 1)
            x = x[15] ? ((x << 1) ^ 16'h1021) : (x << 1);
        crc16_byte = x;
    end
endfunction

// degC -> XADC code: code = (T + 273.15) * 4096 / 503.975
function [11:0] degc_to_code;
    input [7:0] t;
    reg [8:0]  s;
    reg [21:0] p;
    begin
        // * 8323 (8.128 scaled by 2^10) as shifts and adds: no DSP (a DSP with its CLK on a constant
        // stopped route_design at the first stage on F2 and F1 Plus 3 cores, 15/11)
        s = {1'b0, t} + 9'd273;
        p = ({13'd0, s} << 13) + ({13'd0, s} << 7) + ({13'd0, s} << 1) + {13'd0, s};
        degc_to_code = p[21:10];
    end
endfunction

localparam [7:0] C_ENUM = 8'h01, C_JOB = 8'h02, C_CTRL = 8'h03, C_STATUS = 8'h04, C_INFO = 8'h05,
                 C_CLK  = 8'h06;
localparam [7:0] R_CAND = 8'h81, R_STATUS = 8'h82, R_INFO = 8'h83;

// ======================================================================= ms tick
localparam integer MS_DIV = CTL_HZ / 1000;
localparam [7:0]   HASH_BUILT5 = HASH_MHZ / 5;
reg [$clog2(MS_DIV+1)-1:0] ms_cnt;
reg ms_tick;
always @(posedge clk) begin
    ms_tick <= 1'b0;
    if (rst) ms_cnt <= 0;
    else if (ms_cnt == MS_DIV - 1) begin ms_cnt <= 0; ms_tick <= 1'b1; end
    else ms_cnt <= ms_cnt + 1'b1;
end

// ======================================================================= command RX
wire       crx_v;
wire [7:0] crx_d;
uart_rx #(.CLKS_PER_BIT(CLKS_PER_BIT)) u_crx (.clk(clk), .rst(rst), .rx(ci), .valid(crx_v), .data(crx_d));

localparam [3:0] P_IDLE = 4'd0, P_H1 = 4'd1, P_CMD = 4'd2, P_ADDR = 4'd3, P_LEN = 4'd4,
                 P_PAY = 4'd5, P_C1 = 4'd6, P_C2 = 4'd7, P_EXEC = 4'd8;
reg [3:0]  pst;
reg [7:0]  p_cmd, p_addr, p_len, p_idx, p_crc_hi;
reg [15:0] p_crc;
reg [7:0]  pbuf [0:254];
reg [15:0] p_tmo;                     // inter-byte timeout (in bit times / 16)
localparam integer TMO_DIV = CLKS_PER_BIT;   // count bit times
reg [$clog2(TMO_DIV+1)-1:0] tmo_div;

// forwarder byte (possibly modified for ENUM)
reg [7:0] fwd_b;
always @(*) begin
    fwd_b = crx_d;
    if (pst == P_PAY && p_cmd == C_ENUM && p_idx == 8'd0) fwd_b = crx_d + 8'd1;
    if (pst == P_PAY && p_cmd == C_ENUM && p_idx == 8'd1) fwd_b = ~(pbuf[0] + 8'd1);
end

wire       ffo_empty, ffo_full;
wire [7:0] ffo_d;
wire       ctx_ready;
sfifo #(.W(8), .DEPTH_LOG2(5)) u_ffwd (
    .clk(clk), .rst(rst), .wr(crx_v), .din(fwd_b), .rd(!ffo_empty && ctx_ready),
    .dout(ffo_d), .empty(ffo_empty), .full(ffo_full), .count());
uart_tx #(.CLKS_PER_BIT(CLKS_PER_BIT)) u_ctx (
    .clk(clk), .rst(rst), .valid(!ffo_empty), .data(ffo_d), .ready(ctx_ready), .tx(co));

// control state
reg        host_en;
reg [11:0] trip_code, resume_code;
reg [7:0]  wd_sec, wd_cnt_s;
reg [9:0]  wd_ms;
reg        wd_expired;
reg        job_busy;                   // job staged, waiting for ack
(* ASYNC_REG = "TRUE" *) reg [2:0] jack_s;
always @(posedge clk) jack_s <= {jack_s[1:0], job_ack_tgl};

reg        want_status, want_info;
reg        resp_status_done, resp_info_done;
reg        pkt_seen;                   // a valid packet for us arrived (watchdog)

integer jb;
wire addr_ok = (p_addr == 8'hFF) || (enumerated && p_addr == {4'd0, my_pos});

reg  [8:0] cur_mhz;                    // reported hash clock
reg        hash_ok_d;
wire       hash_rise = hash_ok && !hash_ok_d;
wire [7:0] clk_pm = pbuf[0], clk_po = pbuf[1];
wire       clk_ok = (clk_pm >= 8'd24) && (clk_pm <= 8'd57) && (clk_po >= 8'd2) && (clk_po <= 8'd64) &&
                    (clk_pm * 16'd25 <= clk_po * MAX_MHZ);

always @(posedge clk) begin
    pkt_seen <= 1'b0; clk_start <= 1'b0;
    hash_ok_d <= hash_ok;
    if (rst) begin
        cur_mhz <= HASH_MHZ; clk_m <= 6'd0; clk_o <= 7'd0;
        pst <= P_IDLE; enumerated <= 1'b0; my_pos <= 4'd0;
        host_en <= 1'b0; job_tgl <= 1'b0; job_busy <= 1'b0; job_kind <= 1'b0;
        trip_code <= degc_to_code(8'd85); resume_code <= degc_to_code(8'd75);
        wd_sec <= 8'd0;
        want_status <= 1'b0; want_info <= 1'b0;
        p_tmo <= 16'd0; tmo_div <= 0;
    end else begin
        if (hash_rise && job_kind) begin
            // the hash domain was reset by a clock change: deliver the current job again
            job_tgl <= ~job_tgl; job_busy <= 1'b1;
        end else if (job_busy && hash_ok && (jack_s[2] == job_tgl)) job_busy <= 1'b0;
        // inter-byte timeout: 40 bit times without a byte resets the parser
        if (crx_v) begin p_tmo <= 16'd0; tmo_div <= 0; end
        else if (pst != P_IDLE && pst != P_EXEC) begin
            if (tmo_div == TMO_DIV - 1) begin tmo_div <= 0; p_tmo <= p_tmo + 16'd1; end
            else tmo_div <= tmo_div + 1'b1;
            if (p_tmo == 16'd40) pst <= P_IDLE;
        end
        if (pst == P_EXEC) begin
            if (!addr_ok && p_cmd != C_ENUM) begin
                pst <= P_IDLE;
            end else case (p_cmd)
                C_ENUM: begin
                    if (pbuf[1] == ~pbuf[0] && pbuf[0] < 8'd16) begin
                        my_pos <= pbuf[0][3:0]; enumerated <= 1'b1;
                    end
                    pst <= P_IDLE;
                end
                C_JOB: if (!job_busy && !hash_rise) begin
                    if (p_len == 8'd209) begin
                        job_kind   <= 1'b1;
                        job_pos    <= my_pos;
                        job_id     <= pbuf[0];
                        job_target <= {pbuf[1], pbuf[2], pbuf[3], pbuf[4], pbuf[5], pbuf[6], pbuf[7], pbuf[8]};
                        for (jb = 0; jb < 200; jb = jb + 1) job_state[8*jb +: 8] <= pbuf[9 + jb];
                        job_tgl  <= ~job_tgl; job_busy <= 1'b1;
                        pkt_seen <= 1'b1;
                    end
                    pst <= P_IDLE;
                end
                C_CTRL: if (!job_busy && !hash_rise) begin
                    if (p_len == 8'd4) begin
                        host_en <= pbuf[0][0];
                        if (pbuf[0][1]) begin
                            job_kind <= 1'b0; job_tgl <= ~job_tgl; job_busy <= 1'b1;
                        end
                        if (pbuf[1] != 8'd0) trip_code   <= degc_to_code((pbuf[1] > 8'd100) ? 8'd100 : pbuf[1]);
                        if (pbuf[2] != 8'd0) resume_code <= degc_to_code((pbuf[2] > 8'd95)  ? 8'd95  : pbuf[2]);
                        wd_sec <= pbuf[3];
                        pkt_seen <= 1'b1;
                    end
                    pst <= P_IDLE;
                end
                C_CLK: begin
                    if (p_len == 8'd4 && clk_ok && !clk_busy) begin
                        clk_m <= clk_pm[5:0]; clk_o <= clk_po[6:0]; clk_start <= 1'b1;
                        cur_mhz <= {pbuf[3][0], pbuf[2]};
                        pkt_seen <= 1'b1;
                    end
                    pst <= P_IDLE;
                end
                C_STATUS: begin want_status <= 1'b1; pkt_seen <= 1'b1; pst <= P_IDLE; end
                C_INFO:   begin want_info   <= 1'b1; pkt_seen <= 1'b1; pst <= P_IDLE; end
                default:  pst <= P_IDLE;
            endcase
        end else if (crx_v) begin
            case (pst)
                P_IDLE: if (crx_d == 8'hA5) pst <= P_H1;
                P_H1:   pst <= (crx_d == 8'h5A) ? P_CMD : ((crx_d == 8'hA5) ? P_H1 : P_IDLE);
                P_CMD:  begin p_cmd <= crx_d; p_crc <= crc16_byte(16'hFFFF, crx_d); pst <= P_ADDR; end
                P_ADDR: begin p_addr <= crx_d; p_crc <= crc16_byte(p_crc, crx_d); pst <= P_LEN; end
                P_LEN:  begin
                    p_len <= crx_d; p_crc <= crc16_byte(p_crc, crx_d); p_idx <= 8'd0;
                    pst <= (crx_d == 8'd0) ? P_C1 : P_PAY;
                end
                P_PAY:  begin
                    pbuf[p_idx] <= crx_d; p_crc <= crc16_byte(p_crc, crx_d);
                    p_idx <= p_idx + 8'd1;
                    if (p_idx + 8'd1 == p_len) pst <= P_C1;
                end
                P_C1:   begin p_crc_hi <= crx_d; pst <= P_C2; end
                P_C2:   pst <= (p_cmd == C_ENUM || {p_crc_hi, crx_d} == p_crc) ? P_EXEC : P_IDLE;
                default: pst <= P_IDLE;
            endcase
        end
        if (resp_status_done) want_status <= 1'b0;
        if (resp_info_done)   want_info   <= 1'b0;
    end
end

// ======================================================================= watchdog
always @(posedge clk) begin
    if (rst) begin
        wd_expired <= 1'b0; wd_ms <= 10'd0; wd_cnt_s <= 8'd0;
    end else if (pkt_seen) begin
        wd_expired <= 1'b0; wd_ms <= 10'd0; wd_cnt_s <= 8'd0;
    end else if (ms_tick && wd_sec != 8'd0 && !wd_expired) begin
        if (wd_ms == 10'd999) begin
            wd_ms <= 10'd0;
            if (wd_cnt_s + 8'd1 >= wd_sec) wd_expired <= 1'b1;
            wd_cnt_s <= wd_cnt_s + 8'd1;
        end else wd_ms <= wd_ms + 10'd1;
    end
end

// ======================================================================= thermal
reg [7:0] temp_age_ms;
reg [11:0] temp_last;
always @(posedge clk) begin
    if (rst) begin
        thermal_trip <= 1'b1; temp_age_ms <= 8'd0; temp_last <= 12'd0;
    end else begin
        if (temp_stb) begin
            temp_age_ms <= 8'd0;
            temp_last <= temp_code;
            if (temp_code >= trip_code) thermal_trip <= 1'b1;
            else if (temp_code <= resume_code) thermal_trip <= 1'b0;
        end else if (ms_tick) begin
            if (temp_age_ms == 8'd100) thermal_trip <= 1'b1;     // no sensor data: fail safe
            else temp_age_ms <= temp_age_ms + 8'd1;
        end
    end
end

always @(posedge clk) hash_en <= !rst && host_en && !thermal_trip && !wd_expired;

// ======================================================================= candidates & snapshot (CDC)
(* ASYNC_REG = "TRUE" *) reg [2:0] creq_s;
(* ASYNC_REG = "TRUE" *) reg [2:0] sack_s;
always @(posedge clk) begin
    creq_s <= {creq_s[1:0], cand_req_tgl};
    sack_s <= {sack_s[1:0], snap_ack_tgl};
end
wire cand_pending = (creq_s[2] != cand_ack_tgl);
reg  snap_wait;
wire snap_done = snap_wait && ((sack_s[2] == snap_req_tgl) || !hash_ok);   // hash clock down: answer anyway

// ======================================================================= own responses
wire       own_empty, own_full;
wire [7:0] own_d;
wire [6:0] own_cnt;
reg        own_wr;
reg  [7:0] own_din;
wire       own_rd;
sfifo #(.W(8), .DEPTH_LOG2(6)) u_fown (
    .clk(clk), .rst(rst), .wr(own_wr), .din(own_din), .rd(own_rd),
    .dout(own_d), .empty(own_empty), .full(own_full), .count(own_cnt));

reg  [7:0]  rb [0:11];                 // TYPE, POS, payload[10]
reg  [4:0]  rb_i;
reg  [15:0] rb_crc;
reg         rb_busy;
reg  [7:0]  own_pkts_add;

always @(posedge clk) begin
    own_wr <= 1'b0; own_pkts_add <= 8'd0;
    resp_status_done <= 1'b0; resp_info_done <= 1'b0;
    if (rst) begin
        rb_busy <= 1'b0; cand_ack_tgl <= 1'b0; snap_req_tgl <= 1'b0; snap_wait <= 1'b0;
    end else if (!rb_busy) begin
        if (own_cnt <= 7'd48) begin
            if (want_info && !resp_info_done) begin
                rb[0] <= R_INFO; rb[1] <= {4'd0, my_pos};
                rb[2] <= "B"; rb[3] <= "N"; rb[4] <= "D"; rb[5] <= "1";
                rb[6] <= VER_MAJOR; rb[7] <= VER_MINOR; rb[8] <= LANES; rb[9] <= CONTEXTS;
                rb[10] <= cur_mhz[7:0]; rb[11] <= {HASH_BUILT5[6:0], cur_mhz[8]};
                rb_busy <= 1'b1; rb_i <= 5'd0; resp_info_done <= 1'b1;
            end else if (want_status && !resp_status_done && !snap_wait) begin
                // ask the hash domain for a fresh snapshot
                snap_req_tgl <= ~snap_req_tgl; snap_wait <= 1'b1;
            end else if (snap_done) begin
                snap_wait <= 1'b0;
                rb[0] <= R_STATUS; rb[1] <= {4'd0, my_pos};
                rb[2] <= temp_last[7:0]; rb[3] <= {4'd0, temp_last[11:8]};
                rb[4] <= {!hash_ok, host_en, wd_expired, bi, enumerated, snap_job_valid, thermal_trip, hash_en};
                rb[5] <= {5'd0, addr_pins};
                rb[6] <= snap_job_id;
                rb[7] <= snap_iter[7:0]; rb[8] <= snap_iter[15:8];
                rb[9] <= snap_iter[23:16]; rb[10] <= snap_iter[31:24];
                rb[11] <= snap_drops;
                rb_busy <= 1'b1; rb_i <= 5'd0; resp_status_done <= 1'b1;
            end else if (cand_pending && hash_ok) begin
                // cand_data: {id, iter[31:0], idx[15:0], top24}
                rb[0] <= R_CAND; rb[1] <= {4'd0, my_pos};
                rb[2] <= cand_data[79:72];
                rb[3] <= cand_data[47:40]; rb[4] <= cand_data[55:48];
                rb[5] <= cand_data[63:56]; rb[6] <= cand_data[71:64];
                rb[7] <= cand_data[31:24]; rb[8] <= cand_data[39:32];
                rb[9] <= cand_data[7:0];   rb[10] <= cand_data[15:8];
                rb[11] <= cand_data[23:16];
                cand_ack_tgl <= ~cand_ack_tgl;
                rb_busy <= 1'b1; rb_i <= 5'd0;
            end
        end
    end else begin
        // serialise: 5A A5 rb[0..11] crc_hi crc_lo  (rb_i 0..15)
        own_wr <= 1'b1;
        case (rb_i)
            5'd0:  own_din <= 8'h5A;
            5'd1:  begin own_din <= 8'hA5; rb_crc <= 16'hFFFF; end
            5'd14: own_din <= rb_crc[15:8];
            5'd15: own_din <= rb_crc[7:0];
            default: begin own_din <= rb[rb_i - 5'd2]; rb_crc <= crc16_byte(rb_crc, rb[rb_i - 5'd2]); end
        endcase
        rb_i <= rb_i + 5'd1;
        if (rb_i == 5'd15) begin rb_busy <= 1'b0; own_pkts_add <= 8'd1; end
    end
    // while the hash domain restarts its toggles return to 0: follow them, so no stale
    // candidate is reported
    if (!rst && !hash_ok) cand_ack_tgl <= creq_s[2];
end

// ======================================================================= responses from below
wire       rrx_v;
wire [7:0] rrx_d;
uart_rx #(.CLKS_PER_BIT(CLKS_PER_BIT)) u_rrx (.clk(clk), .rst(rst), .rx(ri), .valid(rrx_v), .data(rrx_d));

wire       rif_empty, rif_full;
wire [7:0] rif_d;
wire [6:0] rif_cnt;
reg        rif_wr;
reg  [7:0] rif_din;
wire       rif_rd;
sfifo #(.W(8), .DEPTH_LOG2(6)) u_fri (
    .clk(clk), .rst(rst), .wr(rif_wr), .din(rif_din), .rd(rif_rd),
    .dout(rif_d), .empty(rif_empty), .full(rif_full), .count(rif_cnt));

localparam [2:0] F_IDLE = 3'd0, F_H = 3'd1, F_A5 = 3'd2, F_BODY = 3'd3, F_PAD = 3'd4;
reg [2:0]  fst;
reg [4:0]  f_n;                         // bytes of the current packet handled
reg        f_drop;
reg [7:0]  ri_pkts_add;
reg [15:0] f_tmo;
always @(posedge clk) begin
    rif_wr <= 1'b0; ri_pkts_add <= 8'd0;
    if (rst) begin
        fst <= F_IDLE; f_tmo <= 16'd0; f_drop <= 1'b0; f_n <= 5'd0;
    end else begin
        if (rrx_v) f_tmo <= 16'd0; else if (f_tmo != 16'hFFFF) f_tmo <= f_tmo + 16'd1;
        case (fst)
            F_IDLE: if (rrx_v && rrx_d == 8'h5A) fst <= F_H;
            F_H: if (rrx_v) begin
                if (rrx_d == 8'hA5) begin
                    // reserve room for the whole packet, otherwise drop it
                    f_drop <= (rif_cnt > 7'd48);
                    if (rif_cnt <= 7'd48) begin rif_wr <= 1'b1; rif_din <= 8'h5A; end
                    fst <= F_A5;
                end else if (rrx_d != 8'h5A) fst <= F_IDLE;
            end
            F_A5: begin
                if (!f_drop) begin rif_wr <= 1'b1; rif_din <= 8'hA5; end
                f_n <= 5'd2; fst <= F_BODY;
            end
            F_BODY: begin
                if (rrx_v) begin
                    if (!f_drop) begin rif_wr <= 1'b1; rif_din <= rrx_d; end
                    f_n <= f_n + 5'd1;
                    if (f_n == 5'd15) begin
                        fst <= F_IDLE;
                        if (!f_drop) ri_pkts_add <= 8'd1;
                    end
                end else if (f_tmo > CLKS_PER_BIT * 40) begin
                    fst <= F_PAD;              // truncated packet: pad to keep framing
                end
            end
            F_PAD: begin
                if (!f_drop) begin rif_wr <= 1'b1; rif_din <= 8'h00; end
                f_n <= f_n + 5'd1;
                if (f_n == 5'd15) begin
                    fst <= F_IDLE;
                    if (!f_drop) ri_pkts_add <= 8'd1;
                end
            end
            default: fst <= F_IDLE;
        endcase
    end
end

// ======================================================================= RO arbiter
reg [7:0] own_pkts, ri_pkts;
reg       ro_busy, ro_src;              // ro_src 0 = own, 1 = from below
reg       ro_last;                       // last source served
reg [4:0] ro_n;
wire      rtx_ready;
wire      src_empty = ro_src ? rif_empty : own_empty;
wire [7:0] src_d    = ro_src ? rif_d : own_d;
wire      ro_send   = ro_busy && !src_empty && rtx_ready;
assign own_rd = ro_send && !ro_src;
assign rif_rd = ro_send &&  ro_src;

uart_tx #(.CLKS_PER_BIT(CLKS_PER_BIT)) u_rtx (
    .clk(clk), .rst(rst), .valid(ro_send), .data(src_d), .ready(rtx_ready), .tx(ro));

wire start_own = !ro_busy && own_pkts != 8'd0 && (ro_last || ri_pkts == 8'd0);
wire start_ri  = !ro_busy && !start_own && ri_pkts != 8'd0;
always @(posedge clk) begin
    if (rst) begin
        own_pkts <= 8'd0; ri_pkts <= 8'd0; ro_busy <= 1'b0; ro_last <= 1'b0; ro_src <= 1'b0; ro_n <= 5'd0;
    end else begin
        own_pkts <= own_pkts + own_pkts_add - {7'd0, start_own};
        ri_pkts  <= ri_pkts  + ri_pkts_add  - {7'd0, start_ri};
        if (start_own) begin
            ro_busy <= 1'b1; ro_src <= 1'b0; ro_last <= 1'b0; ro_n <= 5'd0;
        end else if (start_ri) begin
            ro_busy <= 1'b1; ro_src <= 1'b1; ro_last <= 1'b1; ro_n <= 5'd0;
        end else if (ro_send) begin
            ro_n <= ro_n + 5'd1;
            if (ro_n == 5'd15) ro_busy <= 1'b0;
        end
    end
end

endmodule
`default_nettype wire
