// tb_engine.v - bound_engine: 3 core, due lavori di fila (il secondo arriva a meta' di un hash).
// Scrive ogni hash finito e ogni candidato in engine.log; run_engine.py li controlla con il modello.
`timescale 1ns/1ps
module tb_engine;
    parameter integer AW = 6, NCORE = 3, NULTRA = 1, RDLAT = 1, BM2 = 0, MULLAT = 2;
    reg clk = 0; always #2.5 clk = ~clk;
    reg rst = 1, job_load = 0, enable = 1;
    reg [255:0] mid; reg [95:0] tail; reg [31:0] base, thresh;
    wire cv; wire [79:0] cd; wire [31:0] hcount, iter; wire [7:0] sj, sd; wire sv;
    // job_state come lo riempie l'interfaccia di catena: byte i nei bit [8i +: 8]
    reg [1599:0] js; integer bi;
    always @* begin
        js = 1600'd0;
        for (bi = 0; bi < 32; bi = bi + 1) js[8*bi +: 8] = mid[255 - 8*bi -: 8];
        for (bi = 0; bi < 12; bi = bi + 1) js[8*(32+bi) +: 8] = tail[95 - 8*bi -: 8];
        js[8*44 +: 32] = base;
    end
    bound_engine #(.NCORE(NCORE), .NULTRA(NULTRA), .IDW(2), .AW(AW), .RDLAT(RDLAT), .BM2(BM2), .MULLAT(MULLAT)) dut (
        .clk(clk), .rst(rst), .en(enable), .pos(4'd0), .job_load(job_load), .job_id(8'h5a),
        .job_target({thresh, 32'hffffffff}), .job_state(js), .job_clear(1'b0),
        .cand_valid(cv), .cand_ready(1'b1), .cand_data(cd),
        .st_iter(iter), .st_job_id(sj), .st_job_valid(sv), .st_drops(sd), .st_batches(hcount));
    wire [31:0] cn = cd[71:40]; wire [63:0] ch = {cd[23:0], 40'd0};
    reg [255:0] j_mid [0:1]; reg [95:0] j_tail [0:1]; reg [31:0] j_base [0:1];
    integer fd, lg, n, job;
    always @(posedge clk) if (cv) $fdisplay(lg, "C %0d %h %h", job, cn, ch);
    task send(input integer j);
        begin
            job = j; mid <= j_mid[j]; tail <= j_tail[j]; base <= j_base[j]; job_load <= 1;
            @(posedge clk); job_load <= 0;
        end
    endtask
    initial begin
        fd = $fopen("jobs.txt", "r");
        n = $fscanf(fd, "%h %h %h\n", j_mid[0], j_tail[0], j_base[0]);
        n = $fscanf(fd, "%h %h %h\n", j_mid[1], j_tail[1], j_base[1]);
        n = $fscanf(fd, "%h\n", thresh);
        $fclose(fd);
        lg = $fopen("engine.log", "w");
        repeat (4) @(posedge clk); rst <= 0; @(posedge clk);
        send(0);
        wait (hcount >= 40); repeat (777) @(posedge clk);
        send(1); repeat (3) @(posedge clk);
        wait (hcount >= 40);
        enable <= 0; repeat (6000) @(posedge clk);
        $fdisplay(lg, "N %0d", hcount);
        repeat (3000) @(posedge clk);
        $fdisplay(lg, "N %0d", hcount);          // fermo: non deve cambiare
        $fclose(lg); $finish;
    end
    initial begin #2000000000; $display("FAIL timeout"); $finish; end
endmodule
