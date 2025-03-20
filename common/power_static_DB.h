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

#ifndef POWER_STATIC_DB_H
#define POWER_STATIC_DB_H

#include "nextpnr.h"
#include <tuple>
#include <map>

NEXTPNR_NAMESPACE_BEGIN

typedef std::unordered_map<IdString, std::map<int, std::tuple<float, float, float>>> BelStaticPowerMap; // <temperature, <voltage, (base_power, low_power, high_power)>>
                                                                                                        // To convert string to IdString: ctx->id("content");
typedef std::unordered_map<IdString, BelStaticPowerMap> StaticPowerMap; // <BelType, BelStaticPowerMap>

class StaticPowerDB {
    private:
        float chip_base_power_ = 0.0;

        bool is_preset_ = false;
        std::map<short, float> preset_temp_to_total_base_power_; // <temperature, <base_power_0.9v, base_power_1v>>
        std::map<std::pair<short, short>, float> temperature_power_slopes_; // <temperature_range, power_slope>
        StaticPowerMap static_power_DB_; // Contains static power data of bels and muxes in sites
                                         // std::tuple<base_power, low_power, high_power>, in nW

    public:
        StaticPowerDB() = default;
        ~StaticPowerDB() =default;
        bool InsertToPresetTemp(short temp, float power);
        void InitTemperaturePowerSlopes(std::map<std::pair<short, short>, float> & temperature_power_slopes);
        void SetChipBasePower(float base_power) { chip_base_power_ = base_power; }
        void SetPreset(bool val) { is_preset_ = val; }
        float GetChipBasePower() const { return chip_base_power_; }
        std::map<short, float>& GetPresetTempToBasePower () { return preset_temp_to_total_base_power_;}
        std::map<std::pair<short, short>, float>& GetPowerSlopes() { return temperature_power_slopes_; }

        bool IsPreset() const { return is_preset_; }
        StaticPowerMap& GetStaticPowerMap() { return static_power_DB_; }




        void SetBelPowerData(IdString bel_type, IdString temp, int voltage, float base_power, float low_power, float high_power) {
            static_power_DB_[bel_type][temp][voltage] = std::make_tuple(base_power, low_power, high_power);
        }
        bool GetBelBasePower(Context *ctx, IdString bel_type, short temp, int voltage, float &base_power);
        bool GetBelLowPower(Context *ctx, IdString bel, short temp, int voltage, float &low_power);
        bool GetBelHighPower(Context *ctx, IdString bel, short temp, int voltage, float &high_power);
};

NEXTPNR_NAMESPACE_END

#endif // POWER_STATIC_DB_H
