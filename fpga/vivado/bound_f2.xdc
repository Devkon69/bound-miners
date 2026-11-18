## bound_f2.xdc - BlackMiner F2 hash board, BOUND (BoundHash) bitstream. Copied unchanged from proxima-f2/vivado/prox_f2.xdc:
## Pin locations and I/O standards from the vendor's open-source 415t.xdc
## (github.com/black1225/blackminer). FX1_FPGA1_2_P is not used by this design.

## 25 MHz oscillator
set_property PACKAGE_PIN AL29 [get_ports osc_clk]
set_property IOSTANDARD LVCMOS18 [get_ports osc_clk]
create_clock -name osc_clk -period 40.000 [get_ports osc_clk]
## if Vivado complains that AL29 is not a clock-capable pin, uncomment:
#set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets clk_in]

## serial chain
set_property PACKAGE_PIN D34  [get_ports CI]
set_property PACKAGE_PIN AG33 [get_ports CO]
set_property PACKAGE_PIN AH34 [get_ports RI]
set_property PACKAGE_PIN C34  [get_ports RO]
set_property PACKAGE_PIN B34  [get_ports BI]
set_property PACKAGE_PIN AJ34 [get_ports BO]
set_property IOSTANDARD LVCMOS18 [get_ports {CI CO RI RO BI BO}]
## idle-high UART inputs: keep them defined if the line is not driven
set_property PULLUP TRUE [get_ports {CI RI}]

## board address straps and LEDs
set_property PACKAGE_PIN AN33 [get_ports {ADDR[0]}]
set_property PACKAGE_PIN AP33 [get_ports {ADDR[1]}]
set_property PACKAGE_PIN AK32 [get_ports {ADDR[2]}]
set_property PACKAGE_PIN AE34 [get_ports {Xil_LED[0]}]
set_property PACKAGE_PIN AD34 [get_ports {Xil_LED[1]}]
set_property IOSTANDARD LVCMOS18 [get_ports {ADDR[*] Xil_LED[*]}]

## the serial lines are asynchronous to every clock
set_false_path -from [get_ports {CI RI BI ADDR[*]}]
set_false_path -to   [get_ports {CO RO BO Xil_LED[*]}]

## clock-domain crossings (toggle handshakes; data is stable for several
## cycles of the destination clock before it is sampled)
set clk_hash [get_clocks -of_objects [get_pins u_mmcm_h/CLKOUT0]]
set clk_ctl  [get_clocks -of_objects [get_pins u_mmcm_c/CLKOUT0]]
set_max_delay -datapath_only -from $clk_ctl  -to $clk_hash 8.000
set_max_delay -datapath_only -from $clk_hash -to $clk_ctl  8.000

## configuration (schematic note: CFGBVS tied to GND for 1.8 V configuration I/O)
set_property CFGBVS GND [current_design]
set_property CONFIG_VOLTAGE 1.8 [current_design]
## compressed like the vendor bitstreams in /fpgabit (smaller, faster to load)
set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]
