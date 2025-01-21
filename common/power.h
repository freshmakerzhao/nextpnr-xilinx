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
#include <memory>

NEXTPNR_NAMESPACE_BEGIN
class DynamicPowerAnalyzer;
class StaticPowerAnalyzer;

class PowerResult {
    private:
        float static_power_; // in nW
        float dynamic_power_; // in nW
        float total_power_; // in nW
        float junction_temp_; // in celcius

        std::unordered_map<IdString, float> resource_powers_; // in nW
        std::unordered_map<IdString, float> net_powers; // in nW

    // Refert to original NextPNR's report.cc for data dump.

    public:
        void set_junction_temp(float temp) { junction_temp_ = temp; }
        void set_static_power(float power) { static_power_ = power; }
};

class PowerAnalyzer {
    private:
        Context *ctx_;
        DynamicPowerAnalyzer dynamic_power_analyzer_;
        StaticPowerAnalyzer static_power_analyzer_;

    public:
        PowerAnalyzer(Context *ctx, float junction_temp, int v_ddc) 
            : ctx_(ctx){
                dynamic_power_analyzer_ = DynamicPowerAnalyzer(ctx_, junction_temp);
                static_power_analyzer_ = StaticPowerAnalyzer(ctx_, junction_temp, v_ddc);
            };
        bool run();
        void write_power_report(std::ostream &out);
};

class StaticPowerAnalyzer{
    private:
        Context *ctx_;
        float junction_temp_ = -100; // in celcius
        float total_base_static_power_; // in nW
        int v_ddc_ = 0;  // chip core voltage, unit: mV
        StaticPowerDB static_power_DB_;

    public:
        StaticPowerAnalyzer(Context *ctx, float junction_temp, int v_ddc) 
            : ctx_(ctx), junction_temp_(junction_temp), v_ddc_(v_ddc){};
        bool run();
        void calculate_temperature_power_slopes();
        bool estimate_base_power_from_preset_temp();


        float get_bel_base_static_power(IdString bel);
        // void write_power_report(std::ostream &out);
};

class DynamicPowerAnalyzer{
    private:
        Context *ctx_;
        DynamicPowerDB dynamic_power_DB_;
        int v_ddc_ = 0;  // unit: mV

    public:
        DynamicPowerAnalyzer(Context *ctx, int v_ddc) 
            : ctx_(ctx), v_ddc_(v_ddc) {};

        bool run();
        float get_bel_usage(IdString bel_type, int v_ddc);

        // missing function for getting MUX usage
};


NEXTPNR_NAMESPACE_END

#endif // POWER_H
