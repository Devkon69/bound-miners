// bound_ram.v - scratchpad di BoundHash: 2^AW righe da 1024 bit, una porta di scrittura e una di lettura.
// STYLE "block": serie 7 (F2, F1 Ultra, F1 Plus): 114 RAMB36 per core a 4096 righe (4096 x 9).
// STYLE "ultra": UltraScale+ (BCU1525, FK33): 15 URAM288 per core (4096 x 72). Le URAM non si
//                inizializzano e non serve: ogni hash riscrive tutte le righe prima di leggerle.
// RDLAT 1: dato un clock dopo l'indirizzo. RDLAT 2: registro di uscita in piu' (piu' MHz).
// Lettura e scrittura della stessa riga nello stesso clock non capitano mai (il core scrive
// la riga j prima di rileggerla oppure usa il valore che ha gia' in mano).
`default_nettype none
module bound_ram #(
    parameter integer AW    = 12,
    parameter         STYLE = "block",
    parameter integer RDLAT = 1
) (
    input  wire          clk,
    input  wire          we,
    input  wire [AW-1:0] waddr,
    input  wire [1023:0] wdata,
    input  wire [AW-1:0] raddr,
    output wire [1023:0] rdata
);
    (* ram_style = STYLE *) reg [1023:0] mem [0:(1<<AW)-1];
    reg [1023:0] q1, q2;
    always @(posedge clk) begin
        if (we) mem[waddr] <= wdata;
        q1 <= mem[raddr];
        q2 <= q1;
    end
    assign rdata = (RDLAT >= 2) ? q2 : q1;
endmodule
`default_nettype wire
