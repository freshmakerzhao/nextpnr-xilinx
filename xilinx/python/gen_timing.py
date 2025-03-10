# 读取bel_timing.json, 生成timing.json

import argparse
import json


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("bel_timing_json", help="bel_timing.json")
    parser.add_argument("timing_json", help="timing.json")
    args = parser.parse_args()

    with open(args.bel_timing_json, "r") as f:
        bel_timing = json.load(f)
    
    timing_data = {}

    def get_timing_value(timing_data:dict):
        fast_min = int(float(timing_data['FAST_MIN'])*1000)
        fast_max = int(float(timing_data['FAST_MAX'])*1000)
        slow_min = int(float(timing_data['SLOW_MIN'])*1000)
        slow_max = int(float(timing_data['SLOW_MAX'])*1000)
        return [fast_min, fast_max, slow_min, slow_max]
    

    # 准备某些bel的timing数据
    # FF
    if True:
        bel_ff = bel_timing['CLBLL_L']['SLICEL']['ff_init']
        q={}
        q["CLK"] = "CK"
        q["clk_q"] = get_timing_value(bel_ff['ff_init_clk_q:SLICEL'])
        ce = {}
        ce["CLK"] = "CK"
        ce["setup"] = get_timing_value(bel_ff['ff_init_setup_ce_clk:SLICEL'])
        ce["hold"] = get_timing_value(bel_ff['ff_init_hold_ce_clk:SLICEL'])
        d={}
        d["CLK"] = "CK"
        d["setup"] = get_timing_value(bel_ff['ff_init_setup_din_clk:SLICEL'])
        d["hold"] = get_timing_value(bel_ff['ff_init_hold_din_clk:SLICEL'])
        pins={"CE":ce, "D":d, "Q":q, "D":d}
        timing_data['SLICE_FFX']={}
        timing_data['SLICE_FFX']["pins"] = pins
    # LUT
    if True:
        bel_lut6 = bel_timing['CLBLL_L']['SLICEL/A6LUT']['lut6']
        bel_lut5 = bel_timing['CLBLL_L']['SLICEL/A5LUT']['lut5']
        path_a1 = {
            "O6":get_timing_value(bel_lut6['lut6_a1_o6:SLICEL:A6LUT']),
            "O5":get_timing_value(bel_lut5['lut5_a1_o5:SLICEL:A5LUT'])
        }
        path_a2 = {
            "O6":get_timing_value(bel_lut6['lut6_a2_o6:SLICEL:A6LUT']),
            "O5":get_timing_value(bel_lut5['lut5_a2_o5:SLICEL:A5LUT'])
        }
        path_a3 = {
            "O6":get_timing_value(bel_lut6['lut6_a3_o6:SLICEL:A6LUT']),
            "O5":get_timing_value(bel_lut5['lut5_a3_o5:SLICEL:A5LUT'])
        }
        path_a4 = {       
            "O6":get_timing_value(bel_lut6['lut6_a4_o6:SLICEL:A6LUT']),
            "O5":get_timing_value(bel_lut5['lut5_a4_o5:SLICEL:A5LUT'])
        }
        path_a5 = {
            "O6":get_timing_value(bel_lut6['lut6_a5_o6:SLICEL:A6LUT']),
            "O5":get_timing_value(bel_lut5['lut5_a5_o5:SLICEL:A5LUT'])
        }
        path_a6 = {
            "O6":get_timing_value(bel_lut6['lut6_a6_o6:SLICEL:A6LUT']),
        }
        timing_data['SLICE_LUTX']={}
        timing_data['SLICE_LUTX']["paths"] = {
            "A1":path_a1,
            "A2":path_a2,
            "A3":path_a3,
            "A4":path_a4,
            "A5":path_a5,
            "A6":path_a6
        }
    # BUFG
    if True:
        timing_data['BUFGCTRL']={}
        bel_bufg = bel_timing['CLK_BUFG_BOT_R']['BUFGCTRL']['bufgctrl']

        timing_data['BUFGCTRL']["paths"] = {
            "I0":{
                "O":get_timing_value(bel_bufg['bufgctrl_i_o'])
            },
            "I1":{
                "O":get_timing_value(bel_bufg['bufgctrl_i_o'])
            },
        }
    # F7、F8MUX
    if True:
        timing_data['SELMUX2_1']={}
        bel_f7mux = bel_timing['CLBLL_L']['SLICEL/F7AMUX']['selmux2_1']
        timing_data['SELMUX2_1']["paths"] = {
            "0":{
                "OUT":get_timing_value(bel_f7mux['selmux2_1_0_out:SLICEL:F7AMUX'])
            },
            "1":{
                "O":get_timing_value(bel_f7mux['selmux2_1_1_out:SLICEL:F7AMUX'])
            },
            "S0":{
                "O":get_timing_value(bel_f7mux['selmux2_1_s0_out:SLICEL:F7AMUX'])
            }
        }
    # CARRY4
    if True:
        # timing_data['CARRY4']={}
        paths = {}
        slice_data = bel_timing['CLBLL_L']['SLICEL']
        for bel_name, bel_data in slice_data.items():
            if 'carry4' not in bel_name:
                continue
            for bel_mode, bel_mode_data in bel_data.items():
                input_name = bel_mode_data['input']
                output_name = bel_mode_data['output']
                if input_name not in paths:
                    paths[input_name]={}
                elif output_name in paths[input_name]:
                    continue
                paths[input_name][output_name] = get_timing_value(bel_mode_data)
        timing_data['CARRY4']={
            "paths":paths
        }
                
    result = {"timing_data":timing_data}
    with open(args.timing_json, "w") as f:
        json.dump(result, f, indent=4)



            


if __name__ == "__main__":
    main()
