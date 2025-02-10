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

#include "power_static_DB.h"

NEXTPNR_NAMESPACE_BEGIN


bool StaticPowerDB::insert_to_preset_temp(short temp, float power_low_volt, float power_high_volt) {
    if (power_low_volt >= 0 && power_high_volt >= 0) {
        std::pair<float, float> powers = std::make_pair(power_low_volt, power_high_volt);
        std::pair<short, std::pair<float, float>> temp_to_watts = std::make_pair(temp, powers);
        preset_temp_to_total_base_power_.insert(temp_to_watts);
        return true;
    }
    else {
        log_error("Invalid power value: %f\n", power_low_volt >= 0 ? power_high_volt : power_low_volt);
        return false;
    }
}

void StaticPowerDB::init_temperature_power_slopes() {
    // Initialize temperature_power_slopes_ with temperature ranges and power slope
    temperature_power_slopes_.clear();
    std::map<std::pair<short, short>, float>().swap(temperature_power_slopes_);
    temperature_power_slopes_[std::make_pair(-55, 0)] = 0.0;
    temperature_power_slopes_[std::make_pair(0, 40)] = 0.0;
    temperature_power_slopes_[std::make_pair(40, 80)] = 0.0;
    temperature_power_slopes_[std::make_pair(80, 125)] = 0.0;
}

bool StaticPowerDB::get_bel_base_power(IdString bel, short temp, int voltage, float &base_power) {
    
    if (static_power_DB_.find(bel) == static_power_DB_.end())
        return false;
    else if (static_power_DB_[bel].find(temp) == static_power_DB_[bel].end())
        return false;

    base_power = std::get<0>(static_power_DB_[bel][temp][voltage]);
    return true;
}

bool StaticPowerDB::get_bel_low_power(IdString bel, short temp, int voltage, float &low_power) {
    if (static_power_DB_.find(bel) == static_power_DB_.end())
        return false;
    else if (static_power_DB_[bel].find(temp) == static_power_DB_[bel].end())
        return false;

    low_power = std::get<1>(static_power_DB_[temp][bel][voltage]);
    return true;
}

bool StaticPowerDB::get_bel_high_power(IdString bel, short temp, int voltage, float &high_power) {
    if (static_power_DB_.find(bel) == static_power_DB_.end())
        return false;
    else if (static_power_DB_[bel].find(temp) == static_power_DB_[bel].end())
        return false;

    high_power = std::get<2>(static_power_DB_[temp][bel][voltage]);
    return true;
}

NEXTPNR_NAMESPACE_END
