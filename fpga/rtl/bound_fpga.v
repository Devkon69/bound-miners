// bound_fpga.v - everything inside one FPGA except clock generation (BOUND / BoundHash, from prox_fpga.v):
// chain interface + thermal/watchdog (control domain), hashing engine (hash
// domain) and the clock-domain crossings between them.
`default_nettype none
module bound_fpga #(
    parameter integer NCORE        = 7,
    parameter integer NULTRA       = 0,
    parameter integer AW           = 12,     // 12 = mainnet (512 KiB); piu' piccolo solo in simulazione
    parameter integer RDLAT        = 1,
    parameter integer BM2          = 0,
    parameter integer MULLAT       = 2,
    parameter integer HASH_MHZ     = 95,
    parameter integer CTL_HZ       = 100000000,
    parameter integer CLKS_PER_BIT = 868
) (
    input  wire       clk_ctl,
    input  wire       clk_hash,
    input  wire       locked_c,     // control clock stable
    input  wire       locked_h,     // hash clock stable (low while it is reprogrammed)
    // hash-clock MMCM dynamic reconfiguration port (clocked by clk_ctl)
    output wire [6:0]  drp_addr,
    output wire [15:0] drp_di,
    input  wire [15:0] drp_do,
    output wire        drp_en,
    output wire        drp_we,
    input  wire        drp_rdy,
    output wire        mmcm_h_rst,
    // chain
    input  wire       ci,
    output wire       co,
    input  wire       ri,
    output wire       ro,
    input  wire       bi,
    output reg        bo,
    input  wire [2:0] addr_pins,
    output wire [1:0] led,
`ifdef SIM
    input  wire [11:0] sim_temp_code,
`endif
    output wire       hashing        // for LEDs / debug
);


// ------------------------------------------------------------------ resets
(* ASYNC_REG = "TRUE" *) reg [3:0] rc_s = 4'hF;
(* ASYNC_REG = "TRUE" *) reg [3:0] rh_s = 4'hF;
always @(posedge clk_ctl  or negedge locked_c) if (!locked_c) rc_s <= 4'hF; else rc_s <= {rc_s[2:0], 1'b0};
always @(posedge clk_hash or negedge locked_h) if (!locked_h) rh_s <= 4'hF; else rh_s <= {rh_s[2:0], 1'b0};
wire rst_c = rc_s[3];
wire rst_h = rh_s[3];

// ------------------------------------------------------------------ temperature
wire [11:0] temp_code;
wire        temp_stb;
xadc_temp #(.CTL_HZ(CTL_HZ)) u_temp (
    .clk(clk_ctl), .rst(rst_c),
`ifdef SIM
    .sim_code(sim_temp_code),
`endif
    .code(temp_code), .stb(temp_stb));

// ------------------------------------------------------------------ control domain
wire        hash_en_c, job_tgl, job_kind;
wire [3:0]  job_pos;
wire [7:0]  job_id;
wire [63:0] job_target;
wire [1599:0] job_state;
reg         job_ack_tgl;
reg         cand_req_tgl;
reg  [79:0] cand_hold;
wire        cand_ack_tgl;
wire        snap_req_tgl;
reg         snap_ack_tgl;
reg  [31:0] snap_iter; reg [7:0] snap_job_id; reg snap_job_valid; reg [7:0] snap_drops;
wire        thermal_trip, enumerated;
wire [3:0]  my_pos;

// ------------------------------------------------------------------ hash clock control
wire       clk_start, clk_busy;
wire [5:0] clk_m;
wire [6:0] clk_o;
mmcm_drp u_drp (
    .clk(clk_ctl), .rst(rst_c), .start(clk_start), .m(clk_m), .o(clk_o), .busy(clk_busy),
    .daddr(drp_addr), .di(drp_di), .dout(drp_do), .den(drp_en), .dwe(drp_we), .drdy(drp_rdy),
    .mmcm_rst(mmcm_h_rst), .locked(locked_h));

// hash_ok: the hash domain has been running for a while (64 control cycles after lock)
(* ASYNC_REG = "TRUE" *) reg [2:0] hl_s;
reg [6:0] hok_cnt;
always @(posedge clk_ctl) begin
    hl_s <= {hl_s[1:0], locked_h};
    if (rst_c || clk_busy || !hl_s[2]) hok_cnt <= 7'd0;
    else if (!hok_cnt[6]) hok_cnt <= hok_cnt + 7'd1;
end
wire hash_ok = hok_cnt[6];

bound_chain_if #(
    .CLKS_PER_BIT(CLKS_PER_BIT), .CTL_HZ(CTL_HZ), .LANES(NCORE), .HASH_MHZ(HASH_MHZ), .CONTEXTS(AW),
    .VER_MAJOR(8'd1), .VER_MINOR(8'd1)
) u_if (
    .clk(clk_ctl), .rst(rst_c),
    .ci(ci), .co(co), .ri(ri), .ro(ro), .bi(bi), .addr_pins(addr_pins),
    .temp_code(temp_code), .temp_stb(temp_stb),
    .hash_en(hash_en_c),
    .job_tgl(job_tgl), .job_ack_tgl(job_ack_tgl), .job_kind(job_kind), .job_pos(job_pos),
    .job_id(job_id), .job_target(job_target),
    .job_state(job_state),
    .cand_req_tgl(cand_req_tgl), .cand_data(cand_hold), .cand_ack_tgl(cand_ack_tgl),
    .snap_req_tgl(snap_req_tgl), .snap_ack_tgl(snap_ack_tgl),
    .snap_iter(snap_iter), .snap_job_id(snap_job_id), .snap_job_valid(snap_job_valid),
    .snap_drops(snap_drops),
    .thermal_trip(thermal_trip), .enumerated(enumerated), .my_pos(my_pos),
    .clk_start(clk_start), .clk_m(clk_m), .clk_o(clk_o), .clk_busy(clk_busy), .hash_ok(hash_ok)
);

always @(posedge clk_ctl) bo <= bi;          // pass the B chain through unchanged

// ------------------------------------------------------------------ CDC into hash domain
(* ASYNC_REG = "TRUE" *) reg [2:0] en_s;
(* ASYNC_REG = "TRUE" *) reg [2:0] jt_s;
(* ASYNC_REG = "TRUE" *) reg [2:0] ca_s;
(* ASYNC_REG = "TRUE" *) reg [2:0] sr_s;
always @(posedge clk_hash) begin
    en_s <= {en_s[1:0], hash_en_c};
    jt_s <= {jt_s[1:0], job_tgl};
    ca_s <= {ca_s[1:0], cand_ack_tgl};
    sr_s <= {sr_s[1:0], snap_req_tgl};
end
wire en_h = en_s[2] && !rst_h;

reg       job_load_h, job_clear_h;
reg [3:0] pos_h;
always @(posedge clk_hash) begin
    job_load_h <= 1'b0; job_clear_h <= 1'b0;
    if (rst_h) begin
        job_ack_tgl <= 1'b0; pos_h <= 4'd0;
    end else if (jt_s[2] != job_ack_tgl) begin
        // job registers in the control domain are stable until the ack returns
        job_ack_tgl <= jt_s[2];
        if (job_kind) begin job_load_h <= 1'b1; pos_h <= job_pos; end
        else job_clear_h <= 1'b1;
    end
end

// the job fields are sampled one cycle after the toggle was seen, still stable
wire        cand_valid;
wire        cand_ready;
wire [79:0] cand_data;
wire [31:0] st_iter, st_batches;
wire [7:0]  st_job_id, st_drops;
wire        st_job_valid;

bound_engine #(.NCORE(NCORE), .NULTRA(NULTRA), .IDW(3), .AW(AW), .RDLAT(RDLAT), .BM2(BM2), .MULLAT(MULLAT)) u_eng (
    .clk(clk_hash), .rst(rst_h), .en(en_h), .pos(pos_h),
    .job_load(job_load_h), .job_id(job_id), .job_target(job_target),
    .job_state(job_state),
    .job_clear(job_clear_h),
    .cand_valid(cand_valid), .cand_ready(cand_ready), .cand_data(cand_data),
    .st_iter(st_iter), .st_job_id(st_job_id), .st_job_valid(st_job_valid),
    .st_drops(st_drops), .st_batches(st_batches)
);

// candidates: one at a time through a toggle handshake
reg cand_pend;
assign cand_ready = !cand_pend && !rst_h;
always @(posedge clk_hash) begin
    if (rst_h) begin
        cand_pend <= 1'b0; cand_req_tgl <= 1'b0;
    end else begin
        if (cand_pend && (ca_s[2] == cand_req_tgl)) cand_pend <= 1'b0;
        if (cand_valid && cand_ready) begin
            cand_hold <= cand_data; cand_req_tgl <= ~cand_req_tgl; cand_pend <= 1'b1;
        end
    end
end

// status snapshot
always @(posedge clk_hash) begin
    if (rst_h) snap_ack_tgl <= 1'b0;
    else if (sr_s[2] != snap_ack_tgl) begin
        snap_iter <= st_iter; snap_job_id <= st_job_id; snap_job_valid <= st_job_valid;
        snap_drops <= st_drops; snap_ack_tgl <= sr_s[2];
    end
end

// ------------------------------------------------------------------ LEDs
// led[0]: heartbeat (1 Hz), led[1]: on = hashing, fast blink = thermal trip
reg [26:0] hb;
always @(posedge clk_ctl) hb <= hb + 27'd1;
assign hashing = hash_en_c;
assign led[0] = hb[26];
assign led[1] = thermal_trip ? hb[23] : hash_en_c;

endmodule
`default_nettype wire
