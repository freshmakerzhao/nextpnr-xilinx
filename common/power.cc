/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  David Shah <david@symbioticeda.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <random>
#include <algorithm>
#include <set>
#include "nextpnr.h"
#include "power.h"
#include "power_parse_json.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

float GenerateRandomNumber(float lower_limit, float upper_limit) {
    // Initialize random number generator with a random device and a uniform distribution
    // std::random_device rd;  // Random device to seed the generator
    std::mt19937 gen(42); // Mersenne Twister generator
    std::uniform_real_distribution<float> dis(lower_limit, upper_limit); // Uniform distribution between lower_limit and upper_limit

    return dis(gen);  // Generate and return a random number in the specified range
}

int FindSiteFanout(Context *ctx, NetInfo *ni) {
    std::set<SiteCoordinates> sites;

    for (auto user : ni->users) {
        CellInfo *ci = user.cell;
        if (ci->bel == BelId())
            continue;
        
        auto &site = ctx->chip_info->tile_insts[ci->bel.tile].site_insts[ctx->locInfo(ci->bel).bel_data[ci->bel.index].site];
        SiteCoordinates site_coordinates {site.site_x, site.site_y, site.rel_x, site.rel_y, site.inter_x, site.inter_y};
        sites.emplace(site_coordinates);
    }

    return sites.size();
}

bool PowerAnalyzer::LoadPowerData(const std::string &path) {
    PowerJsonReader data_parser(path);
    json jsonData;
    if (!data_parser.LoadData(jsonData)) {
        log_error("Failed to load power data to power analyzer.\n");
        return false;
    }

    // Get voltage standard
    int v_ddc = static_power_analyzer_.GetVddc();
    std::string v_ddc_str = std::to_string(v_ddc);
    

    // Save power data based on voltage
    json v_ddc_jsonData = jsonData[v_ddc_str];
    for (const auto& bel_key : {"bels", "routing_resources"}) {
        for (auto &bel_entry: v_ddc_jsonData[bel_key].object_items()) {
                IdString bel_type = ctx_->id(bel_entry.first);
                for (auto &temp_entry : bel_entry.second["static_power"].object_items()) {
                    IdString temperature = ctx_->id(temp_entry.first); 
                    float base_power = temp_entry.second["base"].number_value();
                    float low_power = temp_entry.second["low"].number_value();
                    float high_power = temp_entry.second["high"].number_value();

                    // 存储到 StaticPowerMap
                    static_power_analyzer_.GetStaticPowerDB().GetStaticPowerMap()[bel_type][temperature][v_ddc] = std::make_tuple(base_power, low_power, high_power);
                }
                for (auto &voltage_entry : bel_entry.second["dynamic_comsumption"].object_items()) {
                    
                    float falling = voltage_entry.second["falling"].number_value();
                    float rising = voltage_entry.second["rising"].number_value();

                    std::unordered_map<IdString, std::map<int, BelDynamicComsumption>>& dynamic_power = dynamic_power_analyzer_.GetDynamicPowerDB().GetDynamicPowerMap();
                    
                    // 存储到 DynamicPowerMap
                    if(bel_key == "bels"){
                        IdString value = ctx_->id(voltage_entry.first); 
                        dynamic_power[bel_type][v_ddc].GetBelConsumptionMap().emplace(value,(falling+rising)/2);
                    }
                    else if(bel_key == "routing_resources"){
                        int value = std::stoi(voltage_entry.first);
                        dynamic_power[bel_type][v_ddc].GetMuxConsumptionMap().emplace(value,(falling+rising)/2);
                        dynamic_power[bel_type][v_ddc].SetMux(true);
                    }

                }
            
        }
    }

    // Save preset static power data
    if (jsonData[v_ddc_str][ctx_->device_name.str(ctx_)].is_object()){
        for(auto& value : jsonData[v_ddc_str][ctx_->device_name.str(ctx_)].object_items()){
            short temperature = static_cast<short>(std::stoi(value.first));
            static_power_analyzer_.GetStaticPowerDB().GetPresetTempToBasePower()[temperature] = static_cast<float>(value.second.number_value());
        }
        static_power_analyzer_.SetPreset(true);
    }
    return true;
}

bool StaticPowerAnalyzer::Run() {
    // Static base power calculation based on preset whole chip power
    if (static_power_DB_.IsPreset()) {
        static_power_DB_.InitTemperaturePowerSlopes(static_power_DB_.GetPowerSlopes());
        CalculateTemperaturePowerSlopes(static_power_DB_.GetPowerSlopes());
        ctx_->power_result.GetPowerSlopes() = static_power_DB_.GetPowerSlopes();
        if (EstimateBasePowerFromPresetTemp())
            return true;
        else
        {
            log_error("Failed to estimate base power from preset temperature.\n");
            return false;
        }
    }
    
    // Static power calculation
    // IdString pad_id = ctx->xc7 ? ctx->id("PAD") : ctx->id("IOB_PAD");
    // for (auto bel : ctx_->getBels()) {
    //     auto bel_type = ctx_->getBelType(bel);  // IdString. bel_type->str(ctx);
    // }

    return true;;
}



//                 *  power value
//                 |
//                 |
//             *   |
//             |   |
//         *   |   |
// *   *   |   |   |
// |   |   |   |   |
// |   |   |   |   |
// *---*---*---*---*  temperature value
// The following function is to calculate the slope of each temperature domain
void StaticPowerAnalyzer::CalculateTemperaturePowerSlopes(std::map<std::pair<short, short>, float>& temperature_power_slopes) {
    // auto &temperature_power_slopes = static_power_DB_.GetPowerSlopes(); //<temperature_range<a,b>, power_slope>
    for (auto &data : temperature_power_slopes) {
        short lower_temp = data.first.first;
        short upper_temp = data.first.second;
        float lower_power = -1.0;
        float upper_power = -1.0;

        std::map<short, float>& preset_power_map = static_power_DB_.GetPresetTempToBasePower();
        for (auto &preset_power : preset_power_map) {
            if (preset_power.first == lower_temp)
                lower_power = preset_power.second;
            else if (preset_power.first == upper_temp)
                upper_power =preset_power.second;
            if (lower_power > 0 && upper_power > 0 ) // if upper and lower values are set, then break.
                break;
        }
        data.second = (upper_power - lower_power) / (upper_temp - lower_temp)*1.0;
    }
}

bool StaticPowerAnalyzer::EstimateBasePowerFromPresetTemp() {
    // Estimate chip_base_power_ at junction_temp_
    auto &temperature_power_slopes = static_power_DB_.GetPowerSlopes();
    auto preset_temp_to_total_base_power = static_power_DB_.GetPresetTempToBasePower();
    for (auto &data : temperature_power_slopes) {
        if (junction_temp_ <= data.first.second && junction_temp_ >= data.first.first) {
            // extract power based on v_ddc_
            auto temp_power = preset_temp_to_total_base_power[data.first.first];
            float chip_base_power = data.second * (junction_temp_ - data.first.first) + temp_power;  // y = ax + b
            static_power_DB_.SetChipBasePower(chip_base_power);
            break;
        }
    }
    // Save chip_base_power_ to PowerResult
    float base_power = static_power_DB_.GetChipBasePower();
    if (base_power > 0) {
        ctx_->power_result.SetStaticPower(base_power);
        ctx_->power_result.AddTotalPower(base_power);
        ctx_->power_result.SetJunctionTemp(junction_temp_);
        return true;
    }
    else {
        log_error("Static base power is negtive: '%f'.\n", base_power);
        return false;
    }
}

bool DynamicPowerAnalyzer::Run(StaticPowerDB &static_power_DB, float temperature) {
    TransitionDensityGenerator();

    // Iterate all nets and calculate dynamic power on each user cell
    for (auto net : sorted(ctx_->nets)) {
        NetInfo *ni = net.second;
        if (ni->driver.cell == nullptr)
            continue;
        if (ni->users.empty())
            continue;
        if (ni->name.str(ctx_).find("GND") != std::string::npos || ni->name.str(ctx_).find("VCC") != std::string::npos)
            continue;

        int site_fanout = FindSiteFanout(ctx_, ni);
        
        // Find clk period
        float clk_period = 0.0;  // in ps, 1e10^-12
        if (ni->is_clk)
            clk_period = ni->clkconstr->period.minDelay();
        else if (ni->capturing_clk != nullptr) 
            clk_period = ni->capturing_clk->clkconstr->period.minDelay();
        else {
            NetInfo *clk_virtual = GetFastGlobelClk(ctx_);
            clk_period = clk_virtual->clkconstr->period.minDelay();
        }
        
        for (auto user : ni->users) {
            CellInfo *ci = user.cell;
            if (ci->bel == BelId())
                continue;
            IdString bel_type = ctx_->getBelType(ci->bel);

            // Get dynamic power based on bel_type
            // IdString bel_pin = ci->pins[user.port];
            // float usage = GetBelUsage(bel_type, bel_pin, v_ddc_);
            float usage = GetBelUsage(bel_type, user.port, v_ddc_);

            // Get net switch density
            float dens = GetTransitionDensity(ctx_, net.first); 

            // Calculate single bel_pin's dynamic power and save result to PowerResult
            float bel_dynamic_power = (usage * std::pow(10, 6)) * dens / clk_period;
            // ctx_->power_result.AddNetPower(net.first, bel_dynamic_power);  // net power is to store routing resource power
            ctx_->power_result.AddDynamicPower(bel_dynamic_power);
            ctx_->power_result.AddTotalPower(bel_dynamic_power);

            //clk_cells
            std::set<IdString> clk_cells = {ctx_->id("BUFGCTRL"), ctx_->id("BUFHCE_BUFHCE"), ctx_->id("BUFIO_BUFIO"), 
                ctx_->id("BUFMRCE_BUFMRCE"), ctx_->id("BUFR_BUFR")}; 
            //IO_cells
            std::set<IdString> IO_cells = {ctx_->id("IOB33M_INBUF_EN"), ctx_->id("IOB33_INBUF_EN"), 
                ctx_->id("IOB33S_INBUF_EN"), ctx_->id("KEEPER"), ctx_->id("IOB33M_OUTBUF"), ctx_->id("IOB33S_OUTBUF"), 
                ctx_->id("IOB33_OUTBUF"), ctx_->id("ODELAYE2_ODELAYE2")};
            //MMCM_cells
            std::set<IdString> MMCM_cells = {ctx_->id("PLLE2_ADV_PLLE2_ADV"), ctx_->id("MMCME2_ADV_MMCME2_ADV")};
            //Logic_cells
            std::set<IdString> Logic_cells = {ctx_->id("SLICE_LUTX"),ctx_->id("CARRY4"),ctx_->id("SLICE_FFX"),
                ctx_->id("ILOGICE3_IFF"),ctx_->id("OLOGICE3_TFF"),ctx_->id("OLOGICE3_OUTFF"),ctx_->id("ISERDESE2_ISERDESE2"),
                ctx_->id("OSERDESE2_OSERDESE2"),ctx_->id("IDELAYE2_IDELAYE2"),ctx_->id("IDELAYCTRL_IDELAYCTRL"),
                ctx_->id("IN_FIFO_IN_FIFO"),ctx_->id("OUT_FIFO_OUT_FIFO"),ctx_->id("IBUFDS_GTE2")};
            //Bram_cells
            std::set<IdString> Bram_cells = {ctx_->id("FIFO18E1_FIFO18E1"),ctx_->id("FIFO36E1_FIFO36E1"),
                ctx_->id("RAMB18E1_RAMB18E1"),ctx_->id("RAMB36E1_RAMB36E1")};
            //Signal_cells
            std::set<IdString> Signal_cells = {ctx_->id("SELMUX2_1")};
            //DSP
            std::set<IdString> DSP_cells = {ctx_->id("DSP48E1_DSP48E1")};
            //GTP
            std::set<IdString> GTP_cells = {ctx_->id("IBUFDS_GTE2"),ctx_->id("GTPE2_COMMON"),ctx_->id("GTPE2_CHANNEL")};
            //GTX
            std::set<IdString> GTX_cells = {ctx_->id("IBUFDS_GTE2"),ctx_->id("GTXE2_COMMON"),ctx_->id("GTXE2_CHANNEL")};

            if (clk_cells.find(ci->type) != clk_cells.end()) {
                auto it = ctx_->power_result.GetClockPowerResult().find(ci->name);
                if (it == ctx_->power_result.GetClockPowerResult().end()) {
                    float frequency = ni->is_clk ? 1.0 / (ni->clkconstr->period.minDelay() * std::pow(10, -6))
                                                    : 1.0 / (ni->capturing_clk->clkconstr->period.minDelay() * std::pow(10, -6));
                    int bel_fanout = ni->users.size();
                    ClockPowerResult clk_result {bel_dynamic_power, ci->name, frequency, ctx_->id("N/A"), ctx_->id("N/A"), ctx_->id("N/A"), bel_fanout, site_fanout, bel_fanout/site_fanout, ctx_->id("N/A")};
                    ctx_->power_result.GetClockPowerResult().insert({ci->name, clk_result});
                } 
                else {
                    it->second.utilization += bel_dynamic_power;
                }
            }
            else if (Logic_cells.find(ci->type) != Logic_cells.end()){
                auto it = ctx_->power_result.GetLogicPowerResult().find(ci->name);
                if (it == ctx_->power_result.GetLogicPowerResult().end()) {
                    //
                    float clock_frequency;
                    float singal_rate;
                    float high_percent;
                    if (ni->is_clk) {
                        clock_frequency = 1.0 / (ni->clkconstr->period.minDelay() * std::pow(10, -6));
                        singal_rate = (transition_density_ / ni->clkconstr->period.minDelay()) * std::pow(10, 6) ;
                        high_percent = signal_probability_ *100 ;
                    }
                    else {
                        clock_frequency = 1.0 / (ni->capturing_clk->clkconstr->period.minDelay() * std::pow(10, -6));
                        singal_rate = (transition_density_ / ni->capturing_clk->clkconstr->period.minDelay()) * std::pow(10, 6) ;
                        high_percent = signal_probability_ * 100 ;
                    }

                    //clock name
                    IdString clock_name;
                    std::unordered_map<std::string,std::string> clk_pins_name;//<cell_type,clk_pins_name>
                    for(auto& port : ci->ports){
                        if(port.second.net && port.second.net->is_clk){
                            clock_name = port.second.net->name;
                            clk_pins_name.emplace(ci->type.str(ctx_),port.first.str(ctx_));
                        }else{
                            clock_name = ctx_->id("Async");
                        }
                    }
                    LogicPowerResult logic_result {bel_dynamic_power, ci->name, ci->type, clock_frequency, clock_name,
                        singal_rate,high_percent};
                    ctx_->power_result.GetLogicPowerResult().insert({ci->name, logic_result});
                } 
                else {
                    it->second.utilization += bel_dynamic_power;
                }
            }
            else if (IO_cells.find(ci->type) != IO_cells.end()){
                auto it = ctx_->power_result.GetIOPowerResult().find(ci->name);
                if (it == ctx_->power_result.GetIOPowerResult().end()) {
                    IOPowerResult IO_result;
                    IO_result.utilization = bel_dynamic_power;
                    ctx_->power_result.GetIOPowerResult().insert({ci->name, IO_result});
                }
                else{
                    it->second.utilization += bel_dynamic_power;
                }
            }
            else if (MMCM_cells.find(ci->type) != MMCM_cells.end()){
                auto it = ctx_->power_result.GetClockManagerPowerResult().find(ci->name);
                if (it == ctx_->power_result.GetClockManagerPowerResult().end()) {
                    ClockManagerPowerResult clockmanager_result;
                    clockmanager_result.utilization = bel_dynamic_power;
                    clockmanager_result.MMCM_OR_PLL = ci->type;
                    ctx_->power_result.GetClockManagerPowerResult().insert({ci->name, clockmanager_result});
                }
                else{
                    it->second.utilization += bel_dynamic_power;
                }
            }
            else if (Bram_cells.find(ci->type) != Bram_cells.end()){
                auto it = ctx_->power_result.GetBRAMPowerResult().find(ci->name);
                if (it == ctx_->power_result.GetBRAMPowerResult().end()) {
                    BRAMPowerResult bram_result;
                    bram_result.utilization = bel_dynamic_power;
                    ctx_->power_result.GetBRAMPowerResult().insert({ci->name, bram_result});
                }
                else{
                    it->second.utilization += bel_dynamic_power;
                }
            }
            else if (Signal_cells.find(ci->type) != Signal_cells.end()){
                auto it = ctx_->power_result.GetSignalsPowerResult().find(ci->name);
                if (it == ctx_->power_result.GetSignalsPowerResult().end()) {
                    SignalsPowerResult signals_result;
                    signals_result.utilization = bel_dynamic_power;
                    ctx_->power_result.GetSignalsPowerResult().insert({ci->name, signals_result});
                }
                else{
                    it->second.utilization += bel_dynamic_power;
                }
            }
            else if (DSP_cells.find(ci->type) != DSP_cells.end()){
                auto it = ctx_->power_result.GetDSPPowerResult().find(ci->name);
                if (it == ctx_->power_result.GetDSPPowerResult().end()) {
                    DSPPowerResult dsp_result;
                    dsp_result.utilization = bel_dynamic_power;
                    ctx_->power_result.GetDSPPowerResult().insert({ci->name, dsp_result});
                }
                else{
                    it->second.utilization += bel_dynamic_power;
                }
            }
            else if(ctx_->device_name == ctx_->id("MC7F100") || ctx_->device_name == ctx_->id("MC7F200")){
                if (GTP_cells.find(ci->type) != GTP_cells.end()){
                    auto it = ctx_->power_result.GetGTManagerPowerResult().find(ci->name);
                    if (it == ctx_->power_result.GetGTManagerPowerResult().end()) {
                        GTManagerPowerResult gtmanager_result;
                        gtmanager_result.utilization = bel_dynamic_power;
                        ctx_->power_result.GetGTManagerPowerResult().insert({ci->name, gtmanager_result});
                    }
                    else{
                        it->second.utilization += bel_dynamic_power;
                    }
                }
            }
            else if(ctx_->device_name == ctx_->id("MC7F160")){
                if (GTX_cells.find(ci->type) != GTX_cells.end()){
                    auto it = ctx_->power_result.GetGTManagerPowerResult().find(ci->name);
                    if (it == ctx_->power_result.GetGTManagerPowerResult().end()) {
                        GTManagerPowerResult gtmanager_result;
                        gtmanager_result.utilization = bel_dynamic_power;
                        ctx_->power_result.GetGTManagerPowerResult().insert({ci->name, gtmanager_result});
                    }
                    else{
                        it->second.utilization += bel_dynamic_power;
                    }
                }
            }

            // Calculate working static power and save result to PowerResult
            float base_power = 0.0;
            float low_power = 0.0;
            float high_power = 0.0;
            static_power_DB.GetBelBasePower(ctx_, bel_type, temperature, v_ddc_, base_power);
            static_power_DB.GetBelLowPower(ctx_, bel_type, temperature, v_ddc_, low_power);
            static_power_DB.GetBelHighPower(ctx_, bel_type, temperature, v_ddc_, high_power);

            float working_static_power = 0.0;
            if (low_power < 0 && high_power < 0)
                continue;
            else if (low_power < 0)
                working_static_power = high_power * signal_probability_ + base_power * (1 - signal_probability_);
            else if (high_power < 0)
                working_static_power = base_power * signal_probability_ + low_power * (1 - signal_probability_);
            else
                working_static_power = high_power * signal_probability_ + low_power * (1 - signal_probability_);
                
            float static_power_diff = working_static_power - base_power;
            ctx_->power_result.AddStaticPower(static_power_diff);
            ctx_->power_result.AddTotalPower(static_power_diff);
        }
    }

    return true;
}

float DynamicPowerAnalyzer::GetBelUsage(IdString bel_type, IdString pin_name, int v_ddc) {
    bool success = false;
    float usage = dynamic_power_DB_.GetBelPowerData(ctx_, bel_type, pin_name, v_ddc, success);
    if (success)
        return usage;
    else {
        log_warning("Failed to get power data of bel '%s' at voltage '%d'.\n", bel_type.c_str(ctx_), v_ddc);
        return 0;
    }
}


void DynamicPowerAnalyzer::TransitionDensityGenerator() {

    for (auto net : sorted(ctx_->nets)) {
        NetInfo *ni = net.second;
        if (ni->driver.cell == nullptr)
            continue;
        if (ni->users.empty())
            continue;

        float shift = GenerateRandomNumber(-0.02, 0.02);
        if (ni->is_clk)
            dynamic_power_DB_.SetTransitionDensity(ni->name, 2);
        else
            dynamic_power_DB_.SetTransitionDensity(ni->name, transition_density_ + shift);
    }
}

bool PowerAnalyzer::Run() {
    if (!LoadPowerData(ctx_->settings[ctx_->id("power_data")].as_string())) {
        log_warning("Failed to load power data.\n");
        return false;
    }
    if (!static_power_analyzer_.Run()) {
        log_warning("Failed to estimate static power.\n");
        return false;
    }
    if (!dynamic_power_analyzer_.Run(static_power_analyzer_.GetStaticPowerDB(), static_power_analyzer_.GetJunctionTemp())) {
        log_warning("Failed to estimate dynamic power.\n");
        return false;
    }
    
    return true;
}

NEXTPNR_NAMESPACE_END
