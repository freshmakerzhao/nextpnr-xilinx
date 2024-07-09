module top (
		input         	clk,
        input           rst_n,
//        input           T,
        output  [3:0]   o_led
    );	
	
	// wire 			clk;
    reg 	[7:0]	ram_out;

	// IBUFDS ibufds_inst (
	// 		.I    		(sys_clk_p	),      // ���ӵ��������
	// 		.IB   		(sys_clk_n	),      // ���ӵ���ָ���
	// 		.O    		(clk		)    	// ��������ź�
	// 	);

////////////////////////////////////////////////////// led output
	reg 	[25:0] 	cnt_led;
//	reg 	[4:0] 	cnt_led;				// for sim
    reg 	[7:0] 	led;
	wire 			flag_cnt_led;

	assign flag_cnt_led = (cnt_led == 0);
	
	always @ (posedge clk or negedge rst_n)
		if (!rst_n)
			begin
				cnt_led <= 1'b0;
				led <= 8'd1;
			end
		else
			begin
				cnt_led <= cnt_led + 1'b1;

				if ((led==8'd1) || (led==8'd2) || (led==8'd4) || (led==8'd8) || (led==8'd16) || (led==8'd32) || (led==8'd64) || (led==8'd128))
					led <= flag_cnt_led ? {led[6:0], led[7]} : led;
				else
					led <= 8'd1; 
			end

//	assign o_led = T ? led : 8'bz;
//	assign o_led = T ? ram_out[7:0] : 8'bz;
	assign o_led = ~{|ram_out[7:6], |ram_out[5:4], |ram_out[3:2], |ram_out[1:0]};

////////////////////////////////////////////////////// ram control
	reg 	[4:0] 	addr;
	reg 			wren;
	reg 			wrdata;
	reg 	[7:0] 	wrdata_buf, rddata_buf;
	wire 			rddata;

	always @ (posedge clk) addr <= {cnt_led[2:0], 2'd0};
//	always @ (posedge clk) addr <= {2'd0, cnt_led[2:0]};				// for sim

	always @ (posedge clk or negedge rst_n)
		if (!rst_n)
			begin
				wren <= 1'b0;
				wrdata_buf <= 8'd0;
				wrdata <= 1'b0;
				rddata_buf <= 8'd0;
				ram_out <= 8'd0;
			end
		else
			case (cnt_led)
			0	:	
				begin
					wren <= 1'b1;
					wrdata_buf <= led;
					wrdata <= led[0];
					ram_out <= rddata_buf;
				end
			1	:	
				begin
					wren <= 1'b1;
					wrdata <= wrdata_buf[1];
					rddata_buf[0] <= rddata;
				end
			2	:	
				begin
					wren <= 1'b1;
					wrdata <= wrdata_buf[2];
					rddata_buf[1] <= rddata;
				end
			3	:	
				begin
					wren <= 1'b1;
					wrdata <= wrdata_buf[3];
					rddata_buf[2] <= rddata;
				end
			4	:	
				begin
					wren <= 1'b1;
					wrdata <= wrdata_buf[4];
					rddata_buf[3] <= rddata;
				end
			5	:	
				begin
					wren <= 1'b1;
					wrdata <= wrdata_buf[5];
					rddata_buf[4] <= rddata;
				end
			6	:	
				begin
					wren <= 1'b1;
					wrdata <= wrdata_buf[6];
					rddata_buf[5] <= rddata;
				end
			7	:	
				begin
					wren <= 1'b1;
					wrdata <= wrdata_buf[7];
					rddata_buf[6] <= rddata;
				end
			8	:	
				begin
					wren <= 1'b0;
					rddata_buf[7] <= rddata;
					ram_out <= {rddata, rddata_buf[6:0]};
				end
			default	:
				begin
					wren <= 1'b0;
					ram_out <= rddata_buf;
				end
			endcase

    RAM32X1S #(
            .INIT(32'h1000_2000)  // Initial contents of RAM
        ) RAM32X1S_inst (
            .O(rddata),   	// 1-bit data output
            .A0(addr[0]), 	// Address[0] input bit
            .A1(addr[1]), 	// Address[1] input bit
            .A2(addr[2]), 	// Address[2] input bit
            .A3(addr[3]), 	// Address[3] input bit
            .A4(addr[4]), 	// Address[4] input bit
            .D(wrdata),  	// 1-bit data input
            .WCLK(clk),    	// Write clock input
            .WE(wren)     	// Write enable input
        );

endmodule
