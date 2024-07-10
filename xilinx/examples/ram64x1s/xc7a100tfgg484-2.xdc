# Clock pin
set_property PACKAGE_PIN R4 [get_ports {sys_clk_p}]
set_property IOSTANDARD DIFF_SSTL15 [get_ports {sys_clk_p}]
set_property PACKAGE_PIN T4 [get_ports {sys_clk_n}]
set_property IOSTANDARD DIFF_SSTL15 [get_ports {sys_clk_n}]

set_property PACKAGE_PIN T6 [get_ports {rst_n}]
set_property IOSTANDARD LVCMOS15 [get_ports {rst_n}]

# LEDs
set_property PACKAGE_PIN C17    [get_ports o_led[0]]
set_property PACKAGE_PIN D17    [get_ports o_led[1]]
set_property PACKAGE_PIN V20    [get_ports o_led[2]]
set_property PACKAGE_PIN U20    [get_ports o_led[3]]
set_property IOSTANDARD LVCMOS33    [get_ports {o_led[0]}]
set_property IOSTANDARD LVCMOS33    [get_ports {o_led[1]}]
set_property IOSTANDARD LVCMOS33    [get_ports {o_led[2]}]
set_property IOSTANDARD LVCMOS33    [get_ports {o_led[3]}]
# Clock constraints
create_clock -period 5.0 [get_ports {sys_clk_p}]
