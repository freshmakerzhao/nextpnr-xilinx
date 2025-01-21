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

#include "power_dynamic_DB.h"

NEXTPNR_NAMESPACE_BEGIN

bool BelDynamicComsumption::add_mux_consumption(int load, float consumption) {
    if (bel_consumption_.size() != 0) {
        log_error("Conflicting bel type. Tried to save bel data, but Mux data already exists.\n");
        return false;
    }
    mux_consumption_[load] = consumption;
    return true
}

float BelDynamicComsumption::get_mux_consumption(int load, bool &success) {
    if (mux_consumption_.find(load) != mux_consumption_.end()){
        success = false;
        log_warning("Failed to get mux consumption of load '%d'.\n", load);
        return 0;
    }
    else {
        success = true;
        return mux_consumption_.at(load);
    }
}

float BelDynamicComsumption::get_bel_consumption(IdString pin_name, bool &success) {
    if (bel_consumption_.find(pin_name) == bel_consumption_.end()) {
        success = false;
        log_warning("Failed to get bel consumption of pin '%s'.\n", pin_name.c_str(ctx));
        return 0;
    }
    else {
        success = true;
        return bel_consumption_.at(pin_name);
    }
}

float DynamicPowerDB::get_bell_power_data(IdString &bel_type, int v_ddc, bool &success) {

    if (dynamic_power_DB_.find(bel_type) == dynamic_power_DB_.end()) {
        success = false;
        log_warning("Failed to get bel power data of bel '%s'.\n", bel_type.c_str(ctx));
        return 0;

    }
    else if (dynamic_power_DB_[bel_type].find(v_ddc) == dynamic_power_DB_[bel_type].end()) {
        success = false;
        log_warning("Failed to get bel power data of bel '%s' at voltage '%d'.\n", bel_type.c_str(ctx), v_ddc);
        return 0;
    }
    
    BelDynamicComsumption bel_data = dynamic_power_DB_[bel_type][v_ddc];
    if (bel_data.is_mux_)  // To-do: add support for mux
        return 0;

    bool success = false;
    float bel_consumption = bel_data.get_bel_consumption("pin_name", success);
    if (success)
        return bel_consumption;
    else {
        log_warning("Failed to get bel consumption of bel '%s'.\n", bel_type.c_str(ctx));
        return 0;
    }
}
NEXTPNR_NAMESPACE_END
