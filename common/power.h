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

#ifndef POWER_H
#define POWER_H

#include "nextpnr.h"
#include "json11.hpp"
#include "power_static_DB.h"
#include "power_dynamic_DB.h"

NEXTPNR_NAMESPACE_BEGIN

float GenerateRandomNumber(float lower_limit, float upper_limit);

class StaticPowerAnalyzer{
    private:
        Context *ctx_;
        float junction_temp_ = -100; // in celcius
        float total_base_static_power_; // in nW
        int v_ddc_ = 0;  // chip core voltage, unit: mV
        StaticPowerDB static_power_DB_;

    public:
        StaticPowerAnalyzer() = default;
        StaticPowerAnalyzer(Context *ctx, float junction_temp, int v_ddc) 
            : ctx_(ctx), junction_temp_(junction_temp), v_ddc_(v_ddc){}
        ~StaticPowerAnalyzer() = default;

        bool Run();
        void CalculateTemperaturePowerSlopes(std::map<std::pair<short, short>, float>& temperature_power_slopes);
        bool EstimateBasePowerFromPresetTemp();
        void SetPreset(bool val) { static_power_DB_.SetPreset(val); }
        StaticPowerDB& GetStaticPowerDB() { return static_power_DB_; }
        int GetVddc() const { return v_ddc_; }
        float GetJunctionTemp() const { return junction_temp_; }

        // float get_bel_base_static_power(IdString bel);
};

class DynamicPowerAnalyzer{
    private:
        Context *ctx_;
        DynamicPowerDB dynamic_power_DB_;
        int v_ddc_ = 0;  // unit: mV
        float transition_density_ = 0.0;
        float signal_probability_ = 0.0;

    public:
        DynamicPowerAnalyzer() = default;
        DynamicPowerAnalyzer(Context *ctx, int v_ddc, float signal_probability, float transition_density) 
            : ctx_(ctx), v_ddc_(v_ddc), signal_probability_(signal_probability), transition_density_(transition_density) {};
        ~DynamicPowerAnalyzer() = default;

        bool Run(StaticPowerDB &static_power_DB, float temperature);
        float GetBelUsage(IdString bel_type, IdString pin_name, int v_ddc);
        void TransitionDensityGenerator();
        float GetTransitionDensity(Context *ctx, IdString net_name) { return dynamic_power_DB_.GetTransitionDensity(ctx, transition_density_, net_name);}
        float GetUserDefinedTransitionDensity() const { return transition_density_;}
        DynamicPowerDB& GetDynamicPowerDB() { return dynamic_power_DB_;}
        // missing function for getting MUX usage
};

class PowerAnalyzer {
    private:
        Context *ctx_;
        DynamicPowerAnalyzer dynamic_power_analyzer_;
        StaticPowerAnalyzer static_power_analyzer_;

    public:
        PowerAnalyzer(Context *ctx, float junction_temp, int v_ddc, float signal_probability, float transition_density) 
            : ctx_(ctx) {
                dynamic_power_analyzer_ = DynamicPowerAnalyzer(ctx_, v_ddc, signal_probability, transition_density);
                static_power_analyzer_ = StaticPowerAnalyzer(ctx_, junction_temp, v_ddc);
        }

        bool Run();
        void WritePowerReport(std::ostream &out);
        bool LoadPowerData(const std::string &path);
};

int FindSiteFanout(Context *ctx, NetInfo *ni);

NEXTPNR_NAMESPACE_END

#endif // POWER_H
