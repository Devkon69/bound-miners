# reclock.tcl - reuse a routed build at a lower hash clock, without recompiling.
#
#   (Vivado Tcl Shell)   cd PERCORSO/fpga/vivado
#                        set argv {7 95 90}; set argc 3; source reclock.tcl          (F2)
#                        set argv {f1u 7 95 90}; set argc 4; source reclock.tcl      (F1 Ultra)
#
#   argv: [BOARD] CORES BUILT_MHZ NEW_MHZ [BM2=0]   (BM2 only names the folder: out_[<BOARD>_]<CORES>c_<MHZ>m[_b1])
#
# Opens out_<CORES>c_<BUILT_MHZ>m/post_route.dcp (routing succeeded, timing not met),
# reprograms the MMCM for NEW_MHZ (the 100 MHz control clock is unchanged), re-checks
# timing and writes out_<CORES>c_<BUILT_MHZ>m_at<NEW_MHZ>/bound_<board>.bit.
# The bitstream still powers up at BUILT_MHZ: start the miner with --freq NEW_MHZ (or lower).

set board f2
if {[llength $argv] > 0 && ![string is integer -strict [lindex $argv 0]]} {
    set board [lindex $argv 0]; set argv [lrange $argv 1 end]
}
set bpre [expr {$board eq "f2" ? "" : "${board}_"}]
set lanes [lindex $argv 0]
set built [lindex $argv 1]
set mhz   [lindex $argv 2]
set mr    [expr {[llength $argv] > 3 ? [lindex $argv 3] : 0}]
set sfx   [expr {$mr ? "_b1" : ""}]

array set mmcm {
    60  {24.0 10.0 10}
    65  {26.0 10.0 10}
    70  {28.0 10.0 10}
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
}
if {![info exists mmcm($mhz)]} { error "unsupported NEW_MHZ $mhz (use: [lsort -integer [array names mmcm]])" }
lassign $mmcm($mhz) mult div0 div1

set here [file normalize [file dirname [info script]]]
set src  [file normalize "$here/out_${bpre}${lanes}c_${built}m${sfx}"]
set out  [file normalize "$here/out_${bpre}${lanes}c_${built}m${sfx}_at${mhz}"]
file mkdir $out
set_param general.maxThreads 8

open_checkpoint $src/post_route.dcp

# worst paths at the original clock, to see what limits the frequency
report_timing -max_paths 30 -nworst 1 -unique_pins -sort_by slack -file $out/worst_paths_at_${built}.rpt
set lane_ff [get_cells -hier -filter {IS_SEQUENTIAL && NAME =~ *g_core*}]
report_timing -to $lane_ff -max_paths 30 -nworst 1 -unique_pins -sort_by slack -file $out/worst_core_paths_at_${built}.rpt
set lslack [get_property SLACK [get_timing_paths -to $lane_ff -max_paths 1 -nworst 1 -setup]]
set aslack [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -setup]]
set period [expr {1000.0 / $built}]
puts [format "AT %d MHz: worst slack everywhere %.3f ns, inside the cores %.3f ns -> cores alone reach about %.0f MHz" \
      $built $aslack $lslack [expr {1000.0 / ($period - $lslack)}]]

set m [get_cells -hier -filter {REF_NAME =~ MMCME2* && NAME =~ *u_mmcm_h*}]
if {[llength $m] == 1} {
    # bitstream v1.1+: separate hash MMCM, the control clock is not touched
    puts "hash MMCM $m: VCO 25 x $mult, hash = VCO / $div0"
    set_property CLKFBOUT_MULT_F  $mult $m
    set_property CLKOUT0_DIVIDE_F $div0 $m
} else {
    set m [get_cells -hier -filter {REF_NAME =~ MMCME2*}]
    if {[llength $m] != 1} { error "expected one MMCM, found [llength $m]" }
    puts "MMCM $m: VCO 25 x $mult, hash = VCO / $div0, ctl = VCO / $div1"
    set_property CLKFBOUT_MULT_F  $mult $m
    set_property CLKOUT0_DIVIDE_F $div0 $m
    set_property CLKOUT1_DIVIDE   $div1 $m
}

report_clocks -file $out/clocks.rpt
report_timing_summary -max_paths 20 -file $out/timing.rpt
set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -setup]]
set whs [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -hold]]
puts "TIMING at ${mhz} MHz: WNS=$wns ns  WHS=$whs ns"
if {$wns < 0 || $whs < 0} {
    puts "ERROR: timing not met at ${mhz} MHz either - try a lower NEW_MHZ. No bitstream written."
    return
}
write_checkpoint -force $out/post_route.dcp
write_bitstream -force -bin_file $out/bound_${board}.bit
puts "DONE: $out/bound_${board}.bit"
puts "hashrate per FPGA = $lanes cores x $mhz MHz x 10^6 / clocks per hash (90660 with BM2 0, RDLAT 1, MULLAT 2)"
