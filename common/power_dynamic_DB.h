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

#ifndef POWER_DYNAMIC_DB_H
#define POWER_DYNAMIC_DB_H

#include "nextpnr.h"
#include <tuple>

NEXTPNR_NAMESPACE_BEGIN

class BelDynamicComsumption;
typedef std::unordered_map<IdString, std::map<int, BelDynamicComsumption>> DynamicPowerMap; // <BelType, <voltage, BelDynamicComsumption>>
                                                                                                      // To convert string to IdString: ctx->id("content");

class BelDynamicComsumption {
    std::unordered_map<int, float> mux_consumption_;    // <Numbe_of_load, consumption>
    std::unordered_map<IdString, float> bel_consumption_; // <PinId, consumption>
    bool is_mux_ = false;

    public:
        BelDynamicComsumption() = default;
        ~BelDynamicComsumption() = default;

        bool AddMuxConsumption(int load, float consumption);
        bool AddBelBonsumption(IdString pin_name, float consumption);

        float GetMuxConsumption(int load, bool &success);
        float GetBelConsumption(Context *ctx, IdString pin_name, bool &success);
        void SetMux(bool val){ is_mux_ = val;}
        std::unordered_map<int, float>& GetMuxConsumptionMap() { return mux_consumption_;}
        std::unordered_map<IdString, float>& GetBelConsumptionMap() { return bel_consumption_;}
        bool IsMux() const { return is_mux_;}
};

class DynamicPowerDB {
    private:
        DynamicPowerMap dynamic_power_DB_;  // Contains dynamic power data of bels and muxes in sites at each temperature
                                            // <bel_type, BelDynamicPowerMap>
        std::unordered_map<IdString, float> net_swtich_densities_; // 
    public:
        DynamicPowerDB() = default;
        ~DynamicPowerDB() = default;

        void SetPowerData(IdString &bel_name, int v_ddc, BelDynamicComsumption &consumption) {
            dynamic_power_DB_[bel_name][v_ddc] = consumption;
        }
        void SetTransitionDensity(IdString net_name, float density);
        float GetBelPowerData(Context *ctx, IdString &bel_name, IdString pin_name, int v_ddc, bool &success);
        float GetTransitionDensity(Context *ctx, float default_density, IdString net_name);
        std::unordered_map<IdString, std::map<int, BelDynamicComsumption>>& GetDynamicPowerMap(){ return dynamic_power_DB_;}
};
NEXTPNR_NAMESPACE_END

#endif // POWER_DYNAMIC_DB_H
