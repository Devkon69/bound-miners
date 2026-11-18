# build.tcl - Vivado non-project build of the BOUND (BoundHash) bitstream for BlackMiner
#             F1 Ultra / F2 / F1 Plus (from proxima-f2: same chain, variable clock and pins).
#
#   (Vivado Tcl Shell NUOVO)  cd PERCORSO/fpga/vivado
#                        set argv {f1u 7 95}; set argc 3; source build.tcl         (F1 Ultra)
#                        set argv {7 95}; set argc 2; source build.tcl             (F2)
#                        set argv {f1p 3 95}; set argc 3; source build.tcl         (F1 Plus)
#
#   argv: [BOARD] [CORES] [MHZ] [PART] [STAGE] [PLACE] [BM2] [RDLAT] [MULLAT]
#   BOARD  optional first word: f2 (default, XC7VX415T-FFG1157, bound_f2.xdc)
#          or f1u (F1 Ultra, XC7K420T-FFG901, bound_f1u.xdc) or f1p (F1 / F1 Plus, XC7K325T-FFG676, bound_f1p.xdc)
#   CORES  BoundHash cores per FPGA: each one needs 114 RAMB36 (512 KiB scratchpad), so at most
#          7 on f2 (880 RAMB36) and f1u (835), 3 on f1p (445). Default 7 (f1p: 3).
#   MHZ    hash clock: 75 80 85 90 95 100 105 110 115 120 125 150 175 200   (default 95)
#          Measured out of context on the F1 Ultra (16/11): 7 cores close at 95,4 MHz with BM2 0.
#   PART   default for the board (check the speed grade printed on the chip)
#   STAGE  all (default) | route : "route" restarts from post_place.dcp of a previous run
#   PLACE  placer directive, default ExtraNetDelay_high (the design is small: 35 % LUT; favour timing)
#   BM2    0 (default) = BlaMka in one clock (8 clocks per round)
#          1 = in two clocks (16 per round): needs about twice the MHz for the same hashrate
#   RDLAT  1 (default) or 2 = scratchpad read latency;  MULLAT 1..4 (default 2) = registers on the 64x64 product
#
# Output in ./out_[<BOARD>_]<CORES>c_<MHZ>m[_b1]/ : bound_<board>.bit/.bin, timing and utilisation reports.
# The build fails loudly if timing is not met (then try reclock.tcl or a lower MHZ).
# hashrate per FPGA = CORES x MHZ x 10^6 / clocks per hash (90.660 with the defaults):
# 7 cores x 95 MHz = 7.335 H/s per FPGA.

# a Tcl Shell that already ran a build keeps its files in memory (MDS 05/11): start clean
catch {close_project}

# optional board as the first word (old F2 form: numbers only)
set board f2
if {[llength $argv] > 0 && ![string is integer -strict [lindex $argv 0]]} {
    set board [lindex $argv 0]; set argv [lrange $argv 1 end]
}
array set defpart { f2 xc7vx415tffg1157-2  f1u xc7k420tffg901-2  f1p xc7k325tffg676-2 }
array set oscpin  { f2 AL29  f1u AG24  f1p AB11 }
if {![info exists defpart($board)]} { error "unknown BOARD $board (use: f2 f1u f1p)" }

array set defcores { f2 7  f1u 7  f1p 3 }
set lanes [expr {[llength $argv] > 0 ? [lindex $argv 0] : $defcores($board)}]
set mhz   [expr {[llength $argv] > 1 ? [lindex $argv 1] : 95}]
set part  [expr {[llength $argv] > 2 ? [lindex $argv 2] : $defpart($board)}]
set stage [expr {[llength $argv] > 3 ? [lindex $argv 3] : "all"}]
set place [expr {[llength $argv] > 4 ? [lindex $argv 4] : "ExtraNetDelay_high"}]
set bm2   [expr {[llength $argv] > 5 ? [lindex $argv 5] : 0}]
set rdlat [expr {[llength $argv] > 6 ? [lindex $argv 6] : 1}]
set mullat [expr {[llength $argv] > 7 ? [lindex $argv 7] : 2}]
if {$lanes > 8} { error "at most 8 cores (3 bits of core id in the nonce)" }
# clocks per hash, as measured in simulation (sim/run_core.py)
set cph [expr {4096 * ((10 + ($rdlat - 1) + 8 * $bm2) + (10 + $mullat + ($rdlat - 1) + 8 * $bm2)) + 548}]

# 25 MHz in: VCO = 25 * MULT (600..1440 MHz), hash = VCO / DIV0, ctl = VCO / DIV1 = 100 MHz
array set mmcm {
    75  {30.0 10.0 10}
    80  {32.0 10.0 10}
    85  {34.0 10.0 10}
    90  {36.0 10.0 10}
    95  {38.0 10.0 10}
    100 {40.0 10.0 10}
    105 {42.0 10.0 10}
    110 {44.0 10.0 10}
    115 {46.0 10.0 10}
    120 {48.0 10.0 10}
    125 {50.0 10.0 10}
    150 {36.0 6.0 9}
    175 {56.0 8.0 14}
    200 {40.0 5.0 10}
}
if {![info exists mmcm($mhz)]} { error "unsupported MHZ $mhz (use: [lsort -integer [array names mmcm]])" }
lassign $mmcm($mhz) mult div0 div1

set here [file normalize [file dirname [info script]]]
set rtl  [file normalize $here/../rtl]
set sfx  [expr {$bm2 ? "_b1" : ""}]
set bpre [expr {$board eq "f2" ? "" : "${board}_"}]
set bitn "bound_${board}"
set out  [file normalize "$here/out_${bpre}${lanes}c_${mhz}m${sfx}"]
file mkdir $out
puts "BOUND $board build: cores=$lanes BM2=$bm2 RDLAT=$rdlat MULLAT=$mullat clk=${mhz}MHz part=$part -> $out"

set_param general.maxThreads 8

if {$stage eq "route"} {
    if {![file exists $out/post_place.dcp]} { error "no $out/post_place.dcp: run the full build first" }
    open_checkpoint $out/post_place.dcp
    set route_dir AggressiveExplore
} else {
    set_part $part
    read_verilog [list \
        $rtl/bound_bm_top.v $rtl/bound_fpga.v $rtl/bound_chain_if.v $rtl/bound_engine.v \
        $rtl/bound_core.v $rtl/bound_ram.v $rtl/bound_sha256.v $rtl/uart.v $rtl/xadc_temp.v $rtl/mmcm_drp.v ]
    read_xdc $here/bound_${board}.xdc

    # no -max_dsp 0 here (Proxima had it): BoundHash needs the DSP48 for its multiplications.
    synth_design -top bound_bm_top -part $part \
        -include_dirs [list $rtl] \
        -generic NCORE=$lanes -generic BM2=$bm2 -generic RDLAT=$rdlat -generic MULLAT=$mullat -generic HASH_MHZ=$mhz \
        -generic MMCM_MULT=$mult -generic MMCM_DIV0=$div0 -generic MMCM_DIV1=$div1 \
        -directive PerformanceOptimized -flatten_hierarchy rebuilt -keep_equivalent_registers \
        -incremental_mode off
    report_utilization -file $out/util_synth.rpt
    report_utilization -hierarchical -hierarchical_depth 4 -file $out/util_synth_hier.rpt
    # pins of THIS board (a Tcl Shell that ran another build may still hold other constraints)
    if {$oscpin($board) ne "" && [get_property PACKAGE_PIN [get_ports osc_clk]] ne $oscpin($board)} {
        error "PIN ERROR: osc_clk on [get_property PACKAGE_PIN [get_ports osc_clk]], expected $oscpin($board) for $board: close Vivado, open it again and retry"
    }
    set nbram [llength [get_cells -hier -filter {REF_NAME =~ RAMB36*}]]
    set ndsp  [llength [get_cells -hier -filter {REF_NAME =~ DSP48*}]]
    puts "CHECK: RAMB36 = $nbram (expected [expr {$lanes * 114}] = 114 per core), DSP48 = $ndsp (about [expr {$lanes * 34}])"
    if {$nbram != $lanes * 114} { puts "WARNING: scratchpad not mapped as 114 RAMB36 per core: look at util_synth_hier.rpt" }
    write_checkpoint -force $out/post_synth.dcp

    opt_design -directive Explore
    # ExtraNetDelay_high favours timing; AltSpreadLogic_high spreads the logic if routing congests
    place_design -directive $place
    phys_opt_design -directive AggressiveExplore
    write_checkpoint -force $out/post_place.dcp
    report_utilization -file $out/util_place.rpt
    set route_dir Explore
}

if {[catch {route_design -directive $route_dir} err]} {
    puts "ROUTE FAILED: $err"
    catch {report_route_status -file $out/route_status.rpt}
    catch {report_design_analysis -congestion -complexity -file $out/congestion.rpt}
    catch {write_checkpoint -force $out/post_route_failed.dcp}
    puts "Reports: $out/route_status.rpt and $out/congestion.rpt"
    puts "Retry the routing only:  set argv {$board $lanes $mhz $part route}; set argc 5; source build.tcl"
    exit 1
}
phys_opt_design -directive AggressiveExplore
write_checkpoint -force $out/post_route.dcp

report_utilization -file $out/util.rpt
report_timing_summary -max_paths 20 -file $out/timing.rpt
report_power -file $out/power.rpt
report_clock_utilization -file $out/clocks.rpt

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -setup]]
set whs [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -hold]]
puts "TIMING: WNS=$wns ns  WHS=$whs ns"
if {$wns < 0 || $whs < 0} {
    puts "ERROR: timing not met at ${mhz} MHz - try reclock.tcl (set argv {$board $lanes $mhz LOWER_MHZ $bm2}) or a lower MHZ. No bitstream written."
    exit 1
}

write_bitstream -force -bin_file $out/${bitn}.bit
puts "DONE: $out/${bitn}.bit and $out/${bitn}.bin"
puts [format "hashrate per FPGA = %d cores x %d MHz / %d clocks per hash = %.0f H/s" $lanes $mhz $cph [expr {$lanes * $mhz * 1e6 / $cph}]]
