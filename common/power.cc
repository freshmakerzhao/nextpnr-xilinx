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
#include "nextpnr.h"
#include "power.h"
#include "power_parse_json.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

float GenerateRandomNumber(float lower_limit, float upper_limit) {
    // Initialize random number generator with a random device and a uniform distribution
    std::random_device rd;  // Random device to seed the generator
    std::mt19937 gen(rd()); // Mersenne Twister generator
    std::uniform_real_distribution<float> dis(lower_limit, upper_limit); // Uniform distribution between lower_limit and upper_limit

    return dis(gen);  // Generate and return a random number in the specified range
}

bool PowerAnalyzer::LoadPowerData(const std::string &path) {
    PowerJsonReader data_parser(path);
    json jsonData;
    if (!data_parser.LoadData(jsonData)) {
        log_error("Failed to estimate power data to power analyzer.\n");
        return false;
    }

    // Get voltage standard
    int v_ddc = static_power_analyzer_.GetVddc();
    std::string v_ddc_str = std::to_string(v_ddc);
    

    // Save power data based on voltage
    json v_ddc_jsonData = jsonData[v_ddc_str];
    for (auto &bel_entry: v_ddc_jsonData["bels"].object_items()) {
            IdString bel_type = ctx_->id(bel_entry.first);
            for (auto &temp_entry : bel_entry.second["static_power"].object_items()) {
                IdString temperature = ctx_->id(temp_entry.first); 
                float base_power = temp_entry.second["base"].number_value();
                float low_power = temp_entry.second["low"].number_value();
                float high_power = temp_entry.second["high"].number_value();

                // 存储到 StaticPowerMap
                static_power_analyzer_.GetStaticPowerDB().GetStaticPowerMap()[bel_type][temperature][v_ddc] = std::make_tuple(base_power, low_power, high_power);
            }
        
    }
    return true;
}

bool StaticPowerAnalyzer::Run() {
    // Static base power calculation based on preset whole chip power
    if (static_power_DB_.IsPreset()) {
        static_power_DB_.InitTemperaturePowerSlopes();
        CalculateTemperaturePowerSlopes();
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
    for (auto bel : ctx_->getBels()) {
        auto bel_type = ctx_->getBelType(bel);  // IdString. bel_type->str(ctx);
    }

    return true;;
}

void StaticPowerAnalyzer::CalculateTemperaturePowerSlopes() {
    auto &temperature_power_slopes = static_power_DB_.GetPowerSlopes(); //<temperature_range<a,b>, power_slope>
    for (auto &data : temperature_power_slopes) {
        short lower_temp = data.first.first;
        short upper_temp = data.first.second;
        float lower_power = -1.0;
        float upper_power = -1.0;

        for (auto &preset_power : static_power_DB_.GetPresetTempToBasePower()) {
            if (preset_power.first == lower_temp)
                lower_power = v_ddc_ == 900? preset_power.second.first : preset_power.second.second;
            else if (preset_power.first == upper_temp)
                upper_power = v_ddc_ == 900? preset_power.second.first : preset_power.second.second;
            if (lower_power * upper_power > 0) // if upper and lower values are set, then break.
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
            auto temp_power = v_ddc_ == 900? preset_temp_to_total_base_power[data.first.first].first : preset_temp_to_total_base_power[data.first.first].second;
            float chip_base_power = data.second * (junction_temp_ - data.first.first) + temp_power;  // y = ax + b
            static_power_DB_.SetChipBasePower(chip_base_power);
            break;
        }
    }
    // Save chip_base_power_ to PowerResult
    float base_power = static_power_DB_.GetChipBasePower();
    if (base_power > 0) {
        ctx_->power_result.SetStaticPower(base_power);
        ctx_->power_result.SetJunctionTemp(junction_temp_);
        return true;
    }
    else {
        log_error("Static base power is negtive: '%f'.\n", base_power);
        return false;
    }
}

bool DynamicPowerAnalyzer::Run(StaticPowerDB &static_power_DB, float temperature) {
    // Iterate all nets and calculate dynamic power on each user cell
    for (auto net : sorted(ctx_->nets)) {
        NetInfo *ni = net.second;
        if (ni->driver.cell == nullptr)
            continue;
        if (ni->users.empty())
            continue;
        float clk_period = 0.0;  // in ns
        if (ni->clkconstr != nullptr) 
            clk_period = ni->clkconstr->period.minDelay();
        else
            continue;  // skip if asynchroneous net
        
        for (auto user : ni->users) {
            CellInfo *ci = user.cell;
            if (ci->bel == BelId())
                continue;
            IdString bel_type = ctx_->getBelType(ci->bel);

            // get dynamic power based on bel_type
            IdString bel_pin = ci->pins[user.port];
            float usage = GetBelUsage(bel_type, bel_pin, v_ddc_);

            float clk_period = ni->clkconstr->period.minDelay(); // clock cycle in ns
            float dens = GetTransitionDensity(ctx_, net.first); // net switch density

            // calculate single bel_pin's dynamic power and save result to PowerResult
            float bel_dynamic_power = (usage * std::pow(10, -6)) * dens / clk_period;
            ctx_->power_result.AddNetPower(net.first, bel_dynamic_power);
            ctx_->power_result.AddResourcePower(bel_type, bel_dynamic_power);
            ctx_->power_result.AddDynamicPower(bel_dynamic_power);
            ctx_->power_result.AddTotalPower(bel_dynamic_power);

            // calculate working static power and save result to PowerResult
            float base_power = 0.0;
            float low_power = 0.0;
            float high_power = 0.0;
            static_power_DB.GetBelBasePower(bel_type, temperature, v_ddc_, base_power);
            static_power_DB.GetBelLowPower(bel_type, temperature, v_ddc_, low_power);
            static_power_DB.GetBelHighPower(bel_type, temperature, v_ddc_, high_power);

            float working_static_power = 0.0;
            if (low_power < 0 && high_power < 0)
                continue;
            else if (low_power < 0)
                working_static_power = base_power * signal_probability_ + low_power * (1 - signal_probability_);
            else if (high_power < 0)
                working_static_power = high_power * signal_probability_ + low_power * (1 - signal_probability_);
            else
                working_static_power = high_power * signal_probability_ + base_power * (1 - signal_probability_);
                
            float static_power_diff = working_static_power - base_power;
            ctx_->power_result.AddStaticPower(static_power_diff);
            ctx_->power_result.AddTotalPower(static_power_diff);
        }
    }
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
        float clk_period = 0.0;  // in ns
        if (ni->clkconstr != nullptr) 
            clk_period = ni->clkconstr->period.minDelay();
        else
            continue;  // skip if asynchroneous net

        float shift = GenerateRandomNumber(-0.02, 0.02);
        if (ni->is_clk)
            dynamic_power_DB_.SetTransitionDensity(ni->name, 2);
        else
            dynamic_power_DB_.SetTransitionDensity(ni->name, transition_density_ + shift);
    }
}

NEXTPNR_NAMESPACE_END
