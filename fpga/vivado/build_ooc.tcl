# build_ooc.tcl - BoundHash (moneta BOUND): sintesi e implementazione "out of context" di bound_engine,
#                 per avere LUT / FF / DSP / BRAM / URAM veri per core e la frequenza che chiude,
#                 prima di attaccare catena seriale / JTAG / UART (passo successivo).
#                 NON produce un bitstream: nessun pin, nessun MMCM.
#
#   (Vivado Tcl Shell NUOVO per ogni build)
#     cd PERCORSO/fpga/vivado
#     set argv {f2}; set argc 1; source build_ooc.tcl                  F2: 7 core, 100 MHz
#     set argv {f2 7 0 175 1 2 3}; set argc 7; source build_ooc.tcl     F2: BlaMka in 2 clock, 175 MHz
#     set argv {bcu 6 3 200 0 2 2 synth}; set argc 8; source build_ooc.tcl   BCU: solo sintesi, 3 URAM + 3 BRAM
#
#   argv: BOARD [NCORE] [NULTRA] [MHZ] [BM2] [RDLAT] [MULLAT] [STAGE] [PART]
#   BOARD   f2 (XC7VX415T) | f1u (XC7K420T) | f1p (XC7K325T) | bcu (XCVU9P) | fk33 (XCVU33P)
#   NCORE   core in totale            (limite RAM: f2 7, f1u 7, f1p 3, bcu 81, fk33 15)
#   NULTRA  quanti con lo scratchpad in URAM (15 URAM288 l'uno); gli altri in BRAM (114 RAMB36 l'uno).
#           Serie 7: sempre 0.  bcu: fino a 63 (21 per SLR).  fk33: fino a 10.
#   MHZ     clock chiesto (vincolo di timing)
#   BM2     0 = BlaMka in 1 clock (8 clock per round), 1 = in 2 clock (16 per round, clock piu' alto)
#   RDLAT   1 o 2 = registri sulla lettura dello scratchpad (2 consigliato con le URAM)
#   MULLAT  1..4 = registri sul prodotto 64x64
#   STAGE   all (default) | synth (solo sintesi: pochi minuti, da' le risorse per core)
#
# Uscita in ./out_<board>_<ncore>c_<mhz>m_b<bm2>r<rdlat>m<mullat>/ : util_*.rpt, timing.rpt, RESULT.txt
# RESULT.txt: WNS, MHz a cui chiuderebbe, clock per hash e hash al secondo stimati.

catch {close_project}
array set defpart { f2 xc7vx415tffg1157-2  f1u xc7k420tffg901-2  f1p xc7k325tffg676-2
                    bcu xcvu9p-fsgd2104-2LV-e  fk33 xcvu33p-fsvh2104-2LV-e }
#                   NCORE NULTRA MHZ BM2 RDLAT MULLAT
array set defs    { f2 {7 0 100 0 1 2}  f1u {7 0 100 0 1 2}  f1p {3 0 100 0 1 2}
                    bcu {6 3 200 0 2 2}  fk33 {15 10 200 0 2 2} }
set board [expr {[llength $argv] > 0 ? [lindex $argv 0] : "f2"}]
if {![info exists defpart($board)]} { error "BOARD sconosciuta $board (f2 f1u f1p bcu fk33)" }
lassign $defs($board) d_nc d_nu d_mhz d_bm2 d_rd d_ml
proc arg {i def} { global argv; if {[llength $argv] > $i} { return [lindex $argv $i] }; return $def }
set ncore  [arg 1 $d_nc]
set nultra [arg 2 $d_nu]
set mhz    [arg 3 $d_mhz]
set bm2    [arg 4 $d_bm2]
set rdlat  [arg 5 $d_rd]
set mullat [arg 6 $d_ml]
set stage  [arg 7 all]
set part   [arg 8 $defpart($board)]
if {$nultra > $ncore} { set nultra $ncore }
set idw 1; while {(1 << $idw) < $ncore} { incr idw }

set here [file normalize [file dirname [info script]]]
set rtl  [file normalize $here/../rtl]
set out  [file normalize "$here/out_${board}_${ncore}c_${mhz}m_b${bm2}r${rdlat}m${mullat}"]
file mkdir $out
puts "BOUND $board: core=$ncore (URAM $nultra) clk=${mhz}MHz BM2=$bm2 RDLAT=$rdlat MULLAT=$mullat part=$part -> $out"
set_param general.maxThreads 8

set_part $part
read_verilog [list $rtl/bound_engine.v $rtl/bound_core.v $rtl/bound_ram.v $rtl/bound_sha256.v]
# -keep_equivalent_registers: i core partono dallo stesso lavoro; senza, la sintesi fonde i registri uguali
synth_design -top bound_engine -part $part -mode out_of_context -include_dirs [list $rtl] \
    -generic NCORE=$ncore -generic NULTRA=$nultra -generic IDW=$idw -generic AW=12 \
    -generic RDLAT=$rdlat -generic BM2=$bm2 -generic MULLAT=$mullat \
    -keep_equivalent_registers -flatten_hierarchy rebuilt
create_clock -name clk -period [expr {1000.0 / $mhz}] [get_ports clk]
write_checkpoint -force $out/post_synth.dcp
report_utilization -file $out/util_synth.rpt
report_utilization -hierarchical -hierarchical_depth 3 -file $out/util_synth_hier.rpt

set fill [expr {10 + ($rdlat - 1) + 8 * $bm2}]
set mix  [expr {10 + $mullat + ($rdlat - 1) + 8 * $bm2}]
set cph  [expr {4096 * ($fill + $mix) + 548}]
set f [open $out/RESULT.txt w]
puts $f "board $board part $part core $ncore (URAM $nultra) BM2 $bm2 RDLAT $rdlat MULLAT $mullat"
puts $f "clock per hash: $cph (fill $fill, mix $mix per passo)"

if {$stage ne "synth"} {
    opt_design
    place_design
    phys_opt_design
    route_design
    phys_opt_design
    write_checkpoint -force $out/post_route.dcp
    report_utilization -file $out/util_route.rpt
    report_timing_summary -file $out/timing.rpt
    report_timing -max_paths 10 -file $out/timing_paths.rpt
    report_power -file $out/power.rpt
    set wns  [get_property SLACK [get_timing_paths -max_paths 1 -setup]]
    set fmax [expr {1000.0 / (1000.0 / $mhz - $wns)}]
    set hps  [expr {$ncore * $fmax * 1e6 / $cph}]
    puts $f [format "clock chiesto %d MHz, WNS %.3f ns -> chiude a %.1f MHz" $mhz $wns $fmax]
    puts $f [format "hashrate stimato: %.0f H/s per FPGA (%.0f H/s per core)" $hps [expr {$hps / $ncore}]]
    puts [format "BOUND %s: WNS %.3f ns, %.1f MHz, %.0f H/s per FPGA" $board $wns $fmax $hps]
} else {
    puts $f "solo sintesi: guarda util_synth.rpt e util_synth_hier.rpt"
    puts "BOUND $board: sintesi finita, vedi $out/util_synth.rpt"
}
close $f
