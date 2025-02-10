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

#include <algorithm>
#include "nextpnr.h"
#include "power.h"
#include "power_parse_json.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN
// using json = json11::Json;
bool PowerAnalyzer::loadPowerData(const std::string &path) {
    PowerJsonReader data_parser(path);
    json jsonData;
    if (!data_parser.loadData(jsonData)) {
        log_error("Failed to estimate power data to power analyzer.\n");
        return false;
    }

    // Get voltage standard
    int v_ddc = static_power_analyzer_.get_vddc();
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
                static_power_analyzer_.getStaticPowerDB().getStaticPowerMap()[bel_type][temperature][v_ddc] = std::make_tuple(base_power, low_power, high_power);
            }
        
    }
    return true;
}

bool StaticPowerAnalyzer::run() {
    // Static base power calculation based on preset whole chip power
    if (static_power_DB_.IsPreset()) {
        static_power_DB_.init_temperature_power_slopes();
        calculate_temperature_power_slopes();
        if (estimate_base_power_from_preset_temp())
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

void StaticPowerAnalyzer::calculate_temperature_power_slopes() {
    auto &temperature_power_slopes = static_power_DB_.get_power_slopes(); //<temperature_range<a,b>, power_slope>
    for (auto &data : temperature_power_slopes) {
        short lower_temp = data.first.first;
        short upper_temp = data.first.second;
        float lower_power = -1.0;
        float upper_power = -1.0;

        for (auto &preset_power : static_power_DB_.get_preset_temp_to_base_power()) {
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

bool StaticPowerAnalyzer::estimate_base_power_from_preset_temp() {
    // Estimate chip_base_power_ at junction_temp_
    auto &temperature_power_slopes = static_power_DB_.get_power_slopes();
    auto preset_temp_to_total_base_power = static_power_DB_.get_preset_temp_to_base_power();
    for (auto &data : temperature_power_slopes) {
        if (junction_temp_ <= data.first.second && junction_temp_ >= data.first.first) {
            // extract power based on v_ddc_
            auto temp_power = v_ddc_ == 900? preset_temp_to_total_base_power[data.first.first].first : preset_temp_to_total_base_power[data.first.first].second;
            float chip_base_power = data.second * (junction_temp_ - data.first.first) + temp_power;  // y = ax + b
            static_power_DB_.set_chip_base_power(chip_base_power);
            break;
        }
    }
    // Save chip_base_power_ to PowerResult
    float base_power = static_power_DB_.get_chip_base_power();
    if (base_power > 0) {
        ctx_->power_result.set_static_power(base_power);
        ctx_->power_result.set_static_power(junction_temp_);
        return true;
    }
    else {
        log_error("Static base power is negtive: '%f'.\n", base_power);
        return false;
    }
}

bool DynamicPowerAnalyzer::run() {
    for (auto net : sorted(ctx_->nets)) {
        NetInfo *ni = net.second;
        if (ni->driver.cell == nullptr)
            continue;
        if (ni->users.empty())
            continue;
        
        for (auto user : ni->users) {
            CellInfo *ci = user.cell;
            if (ci->bel == BelId())
                continue;
            IdString bel_type = ctx_->getBelType(ci->bel);

            // get dynamic power based on bel_type
            IdString bel_pin = ci->pins[user.port];
            float usage = get_bel_usage(bel_type, bel_pin, v_ddc_);

            // get clock cycle in ns
            

            // calculate single bel_pin's dynamic power


            // float base_power = static_power_DB_.get_bel_base_static_power(bel_type);
            // if (base_power < 0) {
            //     log_error("Failed to get base power of bel '%s'.\n", bel_type.c_str(ctx));
            //     return false;
            // }
            // float low_power = static_power_DB_.get_bel_low_static_power(bel_type);
            // if (low_power < 0) {
            //     log_error("Failed to get low power of bel '%s'.\n", bel_type.c_str(ctx));
            //     return false;
            // }
            // float high_power = static_power_DB_.get_bel_high_static_power(bel_type);
            // if (high_power < 0) {
            //     log_error("Failed to get high power of bel '%s'.\n", bel_type.c_str(ctx));
            //     return false;
            // }


            // float dynamic_power = base_power + low_power + high_power;
            // ctx->power_result.net_powers[net.first] += dynamic_power;
            // ctx->power_result.resource_powers[bel_type] += dynamic_power;
        }
    }
}

float DynamicPowerAnalyzer::get_bel_usage(IdString bel_type, IdString pin_name, int v_ddc) {
    bool success = false;
    float usage = dynamic_power_DB_.get_bel_power_data(ctx_, bel_type, pin_name, v_ddc, success);
    if (success)
        return usage;
    else {
        log_warning("Failed to get power data of bel '%s' at voltage '%d'.\n", bel_type.c_str(ctx_), v_ddc);
        return 0;
    }
}

NEXTPNR_NAMESPACE_END
