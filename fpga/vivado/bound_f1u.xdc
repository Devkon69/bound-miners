## bound_f1u.xdc - BlackMiner F1 Ultra hash board, BOUND (BoundHash) bitstream. Copied unchanged from proxima-f2/vivado/prox_f1u.xdc:
## Pin locations and I/O standards from the vendor's open-source 420t.xdc
## (github.com/black1225/blackminer), same as qtc_f1_420t.xdc of the QTC project; copied from ting_f1u.xdc (Ting, working on the machine since 09/11/2026).
## Same pins as the Ting Ultra bitstream that runs on the machine (confirmed by the user 09/11).

## 25 MHz oscillator
set_property PACKAGE_PIN AG24 [get_ports osc_clk]
set_property IOSTANDARD LVCMOS33 [get_ports osc_clk]
create_clock -name osc_clk -period 40.000 [get_ports osc_clk]
## AG24 is not known to be a clock-capable pin: allow the general route to the MMCM
## (as on the F1, harmless if it is clock capable)
set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets clk_in]

## serial chain
set_property PACKAGE_PIN C30 [get_ports CI]
set_property PACKAGE_PIN AG30 [get_ports CO]
set_property PACKAGE_PIN AH30 [get_ports RI]
set_property PACKAGE_PIN B30 [get_ports RO]
set_property PACKAGE_PIN A30 [get_ports BI]
set_property PACKAGE_PIN AK30 [get_ports BO]
set_property IOSTANDARD LVCMOS33 [get_ports {CI CO RI RO BI BO}]
## idle-high UART inputs: keep them defined if the line is not driven
set_property PULLUP TRUE [get_ports {CI RI}]

## board address straps and LEDs
set_property PACKAGE_PIN R30 [get_ports {ADDR[0]}]
set_property PACKAGE_PIN V30 [get_ports {ADDR[1]}]
set_property PACKAGE_PIN U30 [get_ports {ADDR[2]}]
set_property PACKAGE_PIN N30 [get_ports {Xil_LED[0]}]
set_property PACKAGE_PIN M30 [get_ports {Xil_LED[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ADDR[*] Xil_LED[*]}]

## the serial lines are asynchronous to every clock
set_false_path -from [get_ports {CI RI BI ADDR[*]}]
set_false_path -to   [get_ports {CO RO BO Xil_LED[*]}]

## clock-domain crossings (toggle handshakes; data is stable for several
## cycles of the destination clock before it is sampled)
set clk_hash [get_clocks -of_objects [get_pins u_mmcm_h/CLKOUT0]]
set clk_ctl  [get_clocks -of_objects [get_pins u_mmcm_c/CLKOUT0]]
set_max_delay -datapath_only -from $clk_ctl  -to $clk_hash 8.000
set_max_delay -datapath_only -from $clk_hash -to $clk_ctl  8.000

## configuration: bank 0 at 3.3 V (schematic: VCCO_0 on the 3.3 V rail, I/O in LVCMOS33).
## If Vivado's DRC reports a CFGBVS / CONFIG_VOLTAGE conflict, use GND / 1.8 instead.
set_property CFGBVS VCCO [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]
## compressed like the vendor bitstreams in /fpgabit (smaller, faster to load)
set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]
