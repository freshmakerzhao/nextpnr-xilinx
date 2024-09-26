/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019  David Shah <david@symbioticeda.com>
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
#include <boost/optional.hpp>
#include <boost/algorithm/string.hpp>
#include <iterator>
#include <queue>
#include <unordered_set>
#include "cells.h"
#include "chain_utils.h"
#include "design_utils.h"
#include "log.h"
#include "nextpnr.h"
#include "pack.h"
#include "pins.h"

NEXTPNR_NAMESPACE_BEGIN

void XC7Packer::prepare_clocking()
{
    log_info("Preparing clocking...\n");
    std::unordered_map<IdString, IdString> upgrade;
    upgrade[ctx->id("MMCME2_BASE")] = ctx->id("MMCME2_ADV");
    upgrade[ctx->id("PLLE2_BASE")] = ctx->id("PLLE2_ADV");

    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (upgrade.count(ci->type)) {
            IdString new_type = upgrade.at(ci->type);
            ci->type = new_type;
        } else if (ci->type == ctx->id("BUFG")) {
            ci->type = ctx->id("BUFGCTRL");
            rename_port(ctx, ci, ctx->id("I"), ctx->id("I0"));
            tie_port(ci, "CE0", true, true);
            tie_port(ci, "S0", true, true);
            tie_port(ci, "S1", false, true);
            tie_port(ci, "IGNORE0", true, true);
        } else if (ci->type == ctx->id("BUFGCE")) {
            ci->type = ctx->id("BUFGCTRL");
            rename_port(ctx, ci, ctx->id("I"), ctx->id("I0"));
            rename_port(ctx, ci, ctx->id("CE"), ctx->id("CE0"));
            tie_port(ci, "S0", true, true);
            tie_port(ci, "S1", false, true);
            tie_port(ci, "IGNORE0", true, true);
        } else if (ci->type == id_BUFH || ci->type == id_BUFHCE) {
            ci->type = id_BUFHCE_BUFHCE;
            tie_port(ci, "CE", true, true);
        } else if (ci->type == id_BUFR) {
            /** Prepare BUFR's Bel Logic **/
            //ci->type = id_BUFR_BUFR;
            ci->setParam(ctx->id("SIM_DEVICE"), Property("7SERIES"));  //  for HybrdChip, always set to '7SERIES'

            if (ci->ports[ctx->id("CE")].net == nullptr)
                tie_port(ci, "CE", true, false);
            if (ci->ports[ctx->id("CLR")].net == nullptr) 
                tie_port(ci, "CLR", false, false);

            std::unordered_map<IdString, XFormRule> bufr_rules;
            bufr_rules[ctx->id("BUFR")].new_type = ctx->id("BUFR_BUFR");
            bufr_rules[ctx->id("BUFR")].port_xform[ctx->id("CE")] = ctx->id("CE");
            bufr_rules[ctx->id("BUFR")].port_xform[ctx->id("CLR")] = ctx->id("CLR");
            bufr_rules[ctx->id("BUFR")].port_xform[ctx->id("I")] = ctx->id("I");
            bufr_rules[ctx->id("BUFR")].port_xform[ctx->id("O")] = ctx->id("O");
            xform_cell(bufr_rules, ci);
        }
    }
}

void XC7Packer::pack_plls()
{
    log_info("Packing PLLs...\n");

    auto set_default = [](CellInfo *ci, IdString param, const Property &value) {
        if (!ci->params.count(param))
            ci->params[param] = value;
    };

    std::unordered_map<IdString, XFormRule> pll_rules;
    pll_rules[ctx->id("MMCME2_ADV")].new_type = ctx->id("MMCME2_ADV_MMCME2_ADV");
    pll_rules[ctx->id("PLLE2_ADV")].new_type = ctx->id("PLLE2_ADV_PLLE2_ADV");
    generic_xform(pll_rules);
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        // Preplace PLLs to make use of dedicated/short routing paths
        if (ci->type == ctx->id("MMCME2_ADV_MMCME2_ADV") || ci->type == ctx->id("PLLE2_ADV_PLLE2_ADV"))
            try_preplace(ci, ctx->id("CLKIN1"));
        if (ci->type == ctx->id("MMCME2_ADV_MMCME2_ADV")) {
            // Fixup parameters
            for (int i = 1; i <= 2; i++)
                set_default(ci, ctx->id("CLKIN" + std::to_string(i) + "_PERIOD"), Property("0.0"));
            for (int i = 0; i <= 6; i++) {
                set_default(ci, ctx->id("CLKOUT" + std::to_string(i) + "_CASCADE"), Property("FALSE"));
                set_default(ci, ctx->id("CLKOUT" + std::to_string(i) + "_DIVIDE"), Property(1));
                set_default(ci, ctx->id("CLKOUT" + std::to_string(i) + "_DUTY_CYCLE"), Property("0.5"));
                set_default(ci, ctx->id("CLKOUT" + std::to_string(i) + "_PHASE"), Property(0));
                set_default(ci, ctx->id("CLKOUT" + std::to_string(i) + "_USE_FINE_PS"), Property("FALSE"));
            }
            set_default(ci, ctx->id("COMPENSATION"), Property("INTERNAL"));

            // Fixup routing
            if (str_or_default(ci->params, ctx->id("COMPENSATION"), "INTERNAL") == "INTERNAL") {
                disconnect_port(ctx, ci, ctx->id("CLKFBIN"));
                connect_port(ctx, ctx->nets[ctx->id("$PACKER_VCC_NET")].get(), ci, ctx->id("CLKFBIN"));
            }
        }
    }
}

void XC7Packer::pack_gbs()
{
    log_info("Packing global buffers...\n");
    std::unordered_map<IdString, XFormRule> gb_rules;
    gb_rules[ctx->id("BUFGCTRL")].new_type = ctx->id("BUFGCTRL");

    generic_xform(gb_rules);

    // Make sure prerequisites are set up first
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == ctx->id("PS7_PS7"))
            preplace_unique(ci);
    }

    // Preplace global buffers to make use of dedicated/short routing
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == id_BUFGCTRL)
            try_preplace(ci, id_I0);
        if (ci->type == id_BUFG_BUFG)
            try_preplace(ci, id_I);
        if (ci->type == id_BUFHCE_BUFHCE)
            try_preplace(ci, id_I);
        if (ci->type == id_BUFR_BUFR)
            try_preplace(ci, id_I);  // Determine bels for BUFR
    }
}

void XC7Packer::pack_clocking()
{
    pack_plls();
    pack_gbs();
}

void XC7Packer::prepare_clock_region_constraints()
{
    prepare_BUFR_dependants();
}

void XC7Packer::prepare_BUFR_dependants()
{
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *current_cell = cell.second;
        if (current_cell->type == id_BUFR_BUFR)
        {   
            // Get current cell clock region id
            BelId bel = current_cell->bel;
            IdString current_clock_region_id = ctx->chip_info->tile_insts[bel.tile].clock_region;
            
            // Pass current cell region info to output net
            NetInfo *output = current_cell->ports[ctx->id("O")].net;
            output->region = ctx->region[current_clock_region_id].get();

            // Pass BUFR's region info to output users
            std::vector<PortRef> &dependants = output->users;
            for(auto dependant : dependants)
                dependant.cell->region = ctx->region[current_clock_region_id].get();
        }
    }
}

NEXTPNR_NAMESPACE_END
