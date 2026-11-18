// tb_core.v - il core contro il modello: ogni riga di vec.txt = mid tail nonce hash.
`timescale 1ns/1ps
module tb_core;
    parameter integer AW = 6, RDLAT = 1, BM2 = 0, MULLAT = 2, NVEC = 4;
    reg clk = 0; always #2.5 clk = ~clk;
    reg rst = 1, start = 0;
    reg [255:0] mid; reg [95:0] tail; reg [31:0] nonce;
    wire busy, done; wire [255:0] hash; wire [31:0] hn;
    bound_core #(.AW(AW), .RDLAT(RDLAT), .BM2(BM2), .MULLAT(MULLAT)) dut (
        .clk(clk), .rst(rst), .start(start), .mid(mid), .tail(tail), .nonce(nonce),
        .busy(busy), .done(done), .hash(hash), .hash_nonce(hn));
    reg [255:0] v_mid [0:63]; reg [95:0] v_tail [0:63]; reg [31:0] v_nonce [0:63]; reg [255:0] v_hash [0:63];
    integer fd, i, n, bad, cyc, c0;
    always @(posedge clk) cyc <= cyc + 1;
    initial begin
        cyc = 0; bad = 0;
        fd = $fopen("vec.txt", "r");
        for (i = 0; i < NVEC; i = i + 1)
            n = $fscanf(fd, "%h %h %h %h\n", v_mid[i], v_tail[i], v_nonce[i], v_hash[i]);
        $fclose(fd);
        repeat (4) @(posedge clk); rst <= 0; @(posedge clk);
        for (i = 0; i < NVEC; i = i + 1) begin
            mid <= v_mid[i]; tail <= v_tail[i]; nonce <= v_nonce[i]; start <= 1; c0 = cyc;
            @(posedge clk); start <= 0;
            @(posedge done); @(posedge clk);
            if (hash !== v_hash[i] || hn !== v_nonce[i]) begin
                bad = bad + 1; $display("DIVERSO vettore %0d: %h atteso %h", i, hash, v_hash[i]);
            end
            if (i == 0) $display("clock per hash: %0d", cyc - c0);
        end
        if (bad == 0) $display("PASS AW=%0d RDLAT=%0d BM2=%0d MULLAT=%0d (%0d vettori)", AW, RDLAT, BM2, MULLAT, NVEC);
        else $display("FAIL (%0d errori)", bad);
        $finish;
    end
    initial begin #2000000000; $display("FAIL timeout"); $finish; end
endmodule
