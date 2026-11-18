## bound_f1p.xdc - BlackMiner F1 Plus hash board, BOUND (BoundHash) bitstream. Copied unchanged from proxima-f2/vivado/prox_f1p.xdc:
## Pin locations and I/O standards from the vendor's open-source 325t.xdc
## (github.com/black1225/blackminer); board facts from F1_Plus_sch.pdf (P0013 rev E):
##   25.00 MHz single-ended oscillator per FPGA (FPGAn_CLK_SINGEND) on AB11
##   chain, LEDs and address straps in 3.3 V HR banks, oscillator in a 1.8 V bank
##   configuration banks at 3.3 V (CFGBVS tied to VCCO_HR), slave serial (M = 111)
## Copied from ting_f1p.xdc (Ting F1 Plus, 09/11/2026), same constraints as rigor_f2.xdc otherwise. FX1_FPGA1_2_P is not used by this design.

## 25 MHz oscillator
set_property PACKAGE_PIN AB11 [get_ports osc_clk]
set_property IOSTANDARD LVCMOS18 [get_ports osc_clk]
create_clock -name osc_clk -period 40.000 [get_ports osc_clk]
## AB11 is the N pin of a clock-capable pair (IO_L13N_T2_MRCC_33 in the schematic):
## a single-ended clock there cannot use the dedicated clock route to the MMCM.
set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets clk_in]

## serial chain
set_property PACKAGE_PIN W26  [get_ports CI]
set_property PACKAGE_PIN AB26 [get_ports CO]
set_property PACKAGE_PIN AC26 [get_ports RI]
set_property PACKAGE_PIN Y26  [get_ports RO]
set_property PACKAGE_PIN K26  [get_ports BI]
set_property PACKAGE_PIN AD26 [get_ports BO]
set_property IOSTANDARD LVCMOS33 [get_ports {CI CO RI RO BI BO}]
## idle-high UART inputs: keep them defined if the line is not driven
set_property PULLUP TRUE [get_ports {CI RI}]

## board address straps and LEDs
set_property PACKAGE_PIN A17 [get_ports {ADDR[0]}]
set_property PACKAGE_PIN A18 [get_ports {ADDR[1]}]
set_property PACKAGE_PIN A19 [get_ports {ADDR[2]}]
set_property PACKAGE_PIN M26 [get_ports {Xil_LED[0]}]
set_property PACKAGE_PIN N26 [get_ports {Xil_LED[1]}]
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

## configuration: CFGBVS tied to VCCO_HR (3.3 V) on this board
set_property CFGBVS VCCO [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]
## compressed like the vendor bitstreams in /fpgabit (smaller, faster to load)
set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]
