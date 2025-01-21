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
#include <unordered_map>

NEXTPNR_NAMESPACE_BEGIN

class BelDynamicComsumption;
typedef std::unordered_map<IdString, std::map<int, BelDynamicComsumption>> DynamicPowerMap; // <BelType, <voltage, BelDynamicComsumption>>
                                                                                                      // To convert string to IdString: ctx->id("content");

class BelDynamicComsumption {
    std::unordered_map<int, float> mux_consumption_;    // <Numbe_of_load, consumption>
    std::unordered_map<IdString, float> bel_consumption_; // <PinId, consumption>
    bool is_mux_ = false;

    public:
        BelComsumption() = default;
        ~BelComsumption() = default;

        bool add_mux_consumption(int load, float consumption);
        bool add_bel_consumption(IdString pin_name, float consumption);

        float get_mux_consumption(int load, bool &success);
        float get_bel_consumption(IdString pin_name, bool &success);
}

class DynamicPowerDB {
    private:
        DynamicPowerMap dynamic_power_DB_;  // Contains dynamic power data of bels and muxes in sites at each temperature
                                            // <bel_type, BelDynamicPowerMap>
    public:
        DynamicPowerDB = default;
        ~DynamicPowerDB = default;

        void set_power_data(IdString &bel_name, int v_ddc, BelDynamicComsumption &consumption) {
            dynamic_power_DB_[bel_name][v_ddc] = consumption;
        }
        float get_bell_power_data(IdString &bel_name, int v_ddc, bool &success);
};
NEXTPNR_NAMESPACE_END

#endif // POWER_DYNAMIC_DB_H
