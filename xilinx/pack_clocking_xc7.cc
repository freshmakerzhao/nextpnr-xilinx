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
    //设置bufgmux的配置规则，BUFGMUX和BUFGMUX_1将S端口所有配置信息应用到CE0和CE1      
    std::unordered_map<IdString, XFormRule> bufgctrl_rules;
    bufgctrl_rules[ctx->id("BUFGMUX")].new_type = ctx->id("BUFGCTRL");
    bufgctrl_rules[ctx->id("BUFGMUX")].port_multixform[ctx->id("S")] = {ctx->id("CE0"),ctx->id("CE1")};
    bufgctrl_rules[ctx->id("BUFGMUX_1")] = bufgctrl_rules[ctx->id("BUFGMUX")];

    bufgctrl_rules[ctx->id("BUFGMUX_CTRL")].new_type = ctx->id("BUFGCTRL");
    bufgctrl_rules[ctx->id("BUFGMUX_CTRL")].port_multixform[ctx->id("S")] = {ctx->id("S0"),ctx->id("S1")};

    bufgctrl_rules[ctx->id("BUFGCE")].new_type = ctx->id("BUFGCTRL");
    bufgctrl_rules[ctx->id("BUFGCE")].port_xform[ctx->id("I")] = ctx->id("I0");
    bufgctrl_rules[ctx->id("BUFGCE")].port_xform[ctx->id("CE")] = ctx->id("CE0");
    bufgctrl_rules[ctx->id("BUFGCE_1")] = bufgctrl_rules[ctx->id("BUFGCE")];

    bufgctrl_rules[ctx->id("BUFG")].new_type = ctx->id("BUFGCTRL");
    bufgctrl_rules[ctx->id("BUFG")].port_xform[ctx->id("I")] = ctx->id("I0");

    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (upgrade.count(ci->type)) {
            bufgctrl_rules[ci->type].new_type = upgrade.at(ci->type);
        } else if (ci->type == ctx->id("BUFG")) {
            tie_port(ci, "CE0", true, true);
            tie_port(ci, "S0", true, true);
            tie_port(ci, "S1", false, true);
            tie_port(ci, "IGNORE0", true, true);
        } else if (ci->type == ctx->id("BUFGCE") || ci->type == ctx->id("BUFGCE_1")) {
            fold_inverter(ci,"CE");
            int inverter = int_or_default(ci->params,ctx->id("IS_CE_INVERTED"));
            if(inverter)
            {
                //根据DEVICE，设置CE1反相，CE0不反相
                ci->params[ctx->id("IS_CE0_INVERTED")] = Property(1);
                ci->params.erase(ctx->id("IS_CE_INVERTED"));
            }
            tie_port(ci, "S0", true, true);
            tie_port(ci, "S1", false, true);
            tie_port(ci, "IGNORE0", true, true);
            tie_port(ci, "IGNORE1", false, true);
        } else if (ci->type == id_BUFH || ci->type == id_BUFHCE) {
            if (ci->type == id_BUFH) {
                tie_port(ci, "CE", true, true);
            }
            ci->type = id_BUFHCE_BUFHCE;
        } else if (ci->type == ctx->id("BUFGMUX") || ci->type == ctx->id("BUFGMUX_1")) {
            
            //吸收端口前的反相器
            fold_inverter(ci,"S");
            int inverter = int_or_default(ci->params,ctx->id("IS_S_INVERTED"));
            if(inverter)
            {
                //根据DEVICE，设置CE1反相，CE0不反相
                ci->params[ctx->id("IS_CE0_INVERTED")] = Property(0);
                ci->params[ctx->id("IS_CE1_INVERTED")] = Property(1);
                ci->params.erase(ctx->id("IS_S_INVERTED"));
            }
            else{
                //根据DEVICE，设置CE0反相，CE1不反相
                ci->params[ctx->id("IS_CE0_INVERTED")] = Property(1);
                ci->params[ctx->id("IS_CE1_INVERTED")] = Property(0);
            }

            std::string sel_type = str_or_default(ci->params, ctx->id("CLK_SEL_TYPE"), "SYNC");
            if(sel_type == "ASYNC")
            {
                //配置持续高电平，不反相
                tie_port(ci, "S0", true, true);
                tie_port(ci, "S1", true, true);
                //配置IGNORE0和IGNORE1，持续高电平，不反相
                tie_port(ci, "IGNORE0", false, true);
                tie_port(ci, "IGNORE1", false, true);

            }
            else if(sel_type == "SYNC"){
                /*这些设置确保BUFGCTRL的行为模拟BUFGMUX:
                    1.S0和S1设为高电平使得CE0和CE1控制输入选择。
                    2.IGNORE0和IGNORE1设为高电平，内部默认反相，所以不需要再反相。*/
                //配置持续高电平，不反相
                tie_port(ci, "S0", true, true);
                tie_port(ci, "S1", true, true);
                //配置IGNORE0和IGNORE1，持续高电平，反相
                tie_port(ci, "IGNORE0", true, false);
                tie_port(ci, "IGNORE1", true, false);
            }

        }else if(ci->type == ctx->id("BUFGMUX_CTRL")){
            
            //吸收端口前的反相器
            fold_inverter(ci,"S");
            for(auto& it : ci->params)
            {
                std::string str = it.first.c_str(ctx);
            }
            int inverter = int_or_default(ci->params,ctx->id("IS_S_INVERTED"));
            if(inverter)
            {
                //根据DEVICE，设置S1反相，S0不反相
                ci->params[ctx->id("IS_S0_INVERTED")] = Property(0);
                ci->params[ctx->id("IS_S1_INVERTED")] = Property(1);
                ci->params.erase(ctx->id("IS_S_INVERTED"));
            }
            else{
                //根据DEVICE，设置S0反相，S1不反相
                ci->params[ctx->id("IS_S0_INVERTED")] = Property(1);
                ci->params[ctx->id("IS_S1_INVERTED")] = Property(0);
            }
            /*这些设置确保BUFGCTRL的行为模拟BUFGMUX_CTRL:
                1.CE0和CE1设为高电平使得S0和S1控制输入选择。
                2.IGNORE0和IGNORE1设为高电平，内部默认反相，所以不需要再反相.*/
            //配置持续高电平，不反相
            tie_port(ci, "CE0", true, true);
            tie_port(ci, "CE1", true, true);
            //配置IGNORE0和IGNORE1，持续高电平，不进入"IS_IGNOREX_INVERTED"表示反相
            tie_port(ci, "IGNORE0", true, false);
            tie_port(ci, "IGNORE1", true, false);
        } else if (ci->type == id_BUFR) {
            /** Prepare BUFR's Bel Logic **/
            //ci->type = id_BUFR_BUFR;
            ci->setParam(ctx->id("SIM_DEVICE"), Property("7SERIES"));  //  for HybrdChip, always set to '7SERIES'

            if (ci->ports[ctx->id("CE")].net == nullptr)
                tie_port(ci, "CE", true, false);
            if (ci->ports[ctx->id("CLR")].net == nullptr) 
                tie_port(ci, "CLR", false, false);

            std::unordered_map<IdString, XFormRule> bufr_rules;
            bufr_rules[id_BUFR].new_type = id_BUFR_BUFR;
            bufr_rules[id_BUFR].port_xform[id_CE] = id_CE;
            bufr_rules[id_BUFR].port_xform[id_CLR] = id_CLR;
            bufr_rules[id_BUFR].port_xform[id_I] = id_I;
            bufr_rules[id_BUFR].port_xform[id_O] = id_O;
            xform_cell(bufr_rules, ci);
        } else if (ci->type == id_BUFIO) {
            std::unordered_map<IdString, XFormRule> bufio_rules;
            bufio_rules[id_BUFIO].new_type = id_BUFIO_BUFIO;
            bufio_rules[id_BUFIO].port_xform[id_I] = id_I;
            bufio_rules[id_BUFIO].port_xform[id_O] = id_O;
            xform_cell(bufio_rules, ci);
        } else if (ci->type == id_BUFMR) {
            std::unordered_map<IdString, XFormRule> bufmr_rules;
            bufmr_rules[id_BUFMR].new_type = id_BUFMRCE_BUFMRCE;
            bufmr_rules[id_BUFMR].port_xform[id_I] = id_I;
            bufmr_rules[id_BUFMR].port_xform[id_O] = id_O;
            tie_port(ci, "CE", true, false);
            xform_cell(bufmr_rules, ci);
        } else if (ci->type == id_BUFMRCE) {
            std::unordered_map<IdString, XFormRule> bufmrce_rules;
            bufmrce_rules[id_BUFMRCE].new_type = id_BUFMRCE_BUFMRCE;
            bufmrce_rules[id_BUFMRCE].port_xform[id_I] = id_I;
            bufmrce_rules[id_BUFMRCE].port_xform[id_O] = id_O;
            bufmrce_rules[id_BUFMRCE].port_xform[id_CE] = id_CE;
            xform_cell(bufmrce_rules, ci);
        } else if (ci->type == id_PHASER_IN_PHY) {
            std::unordered_map<IdString, XFormRule> phaser_rules;
            phaser_rules[id_PHASER_IN_PHY].new_type = id_PHASER_IN_PHY_PHASER_IN_PHY;
            xform_cell(phaser_rules, ci);
        }
    }
    //应用bufgmux_rules中的规则
    generic_xform(bufgctrl_rules);
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
        if (ci->type == id_BUFIO_BUFIO)
            try_preplace(ci, id_I);  // Determine bels for BUFR

    }
}

void XC7Packer::pack_clocking()
{
    pack_plls();
    pack_gbs();
}


NEXTPNR_NAMESPACE_END
