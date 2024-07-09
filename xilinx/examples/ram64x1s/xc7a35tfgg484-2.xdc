# Clock pin
set_property PACKAGE_PIN W19 [get_ports {clk}]
set_property IOSTANDARD LVCMOS33 [get_ports {clk}]

set_property PACKAGE_PIN Y19 [get_ports {rst_n}]
set_property IOSTANDARD LVCMOS33 [get_ports {rst_n}]

# LEDs
set_property PACKAGE_PIN N20    [get_ports o_led[0]]
set_property IOSTANDARD LVCMOS33    [get_ports {o_led[0]}]
set_property PACKAGE_PIN M20    [get_ports o_led[1]]
set_property IOSTANDARD LVCMOS33    [get_ports {o_led[1]}]
set_property PACKAGE_PIN N22    [get_ports o_led[2]]
set_property IOSTANDARD LVCMOS33    [get_ports {o_led[2]}]
set_property PACKAGE_PIN M22    [get_ports o_led[3]]
set_property IOSTANDARD LVCMOS33    [get_ports {o_led[3]}]

# Clock constraints
create_clock -period 20.0 [get_ports {clk}]
