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

#include "pack.h"
#include <algorithm>
#include <boost/optional.hpp>
#include <iterator>
#include <queue>
#include <unordered_set>
#include "cells.h"
#include "chain_utils.h"
#include "design_utils.h"
#include "log.h"
#include "nextpnr.h"
#include "pins.h"

NEXTPNR_NAMESPACE_BEGIN

// Process the contents of packed_cells and new_cells
void XilinxPacker::flush_cells()
{
    for (auto pcell : packed_cells) {
        for (auto &port : ctx->cells[pcell]->ports) {
            disconnect_port(ctx, ctx->cells[pcell].get(), port.first);
        }
        ctx->cells.erase(pcell);
    }
    for (auto &ncell : new_cells) {
        NPNR_ASSERT(!ctx->cells.count(ncell->name));
        ctx->cells[ncell->name] = std::move(ncell);
    }
    packed_cells.clear();
    new_cells.clear();
}

void XilinxPacker::xform_cell(const std::unordered_map<IdString, XFormRule> &rules, CellInfo *ci)
{
    auto &rule = rules.at(ci->type);
    ci->attrs[ctx->id("X_ORIG_TYPE")] = ci->type.str(ctx);
    ci->type = rule.new_type;
    std::vector<IdString> orig_port_names;
    for (auto &port : ci->ports)
        orig_port_names.push_back(port.first);

    for (auto pname : orig_port_names) {
        if (rule.port_multixform.count(pname)) {
            auto old_port = ci->ports.at(pname);
            disconnect_port(ctx, ci, pname);
            ci->ports.erase(pname);
            for (auto new_name : rule.port_multixform.at(pname)) {
                ci->ports[new_name].name = new_name;
                ci->ports[new_name].type = old_port.type;
                connect_port(ctx, old_port.net, ci, new_name);
                ci->attrs[ctx->id("X_ORIG_PORT_" + new_name.str(ctx))] = pname.str(ctx);
            }
        } else {
            IdString new_name;
            if (rule.port_xform.count(pname)) {
                new_name = rule.port_xform.at(pname);
            } else {
                std::string stripped_name;
                for (auto c : pname.str(ctx))
                    if (c != '[' && c != ']')
                        stripped_name += c;
                new_name = ctx->id(stripped_name);
            }
            if (new_name != pname) {
                rename_port(ctx, ci, pname, new_name);
            }
            ci->attrs[ctx->id("X_ORIG_PORT_" + new_name.str(ctx))] = pname.str(ctx);
        }
    }

    std::vector<IdString> xform_params;
    for (auto &param : ci->params)
        if (rule.param_xform.count(param.first))
            xform_params.push_back(param.first);
    for (auto param : xform_params)
        ci->params[rule.param_xform.at(param)] = ci->params[param];

    for (auto &attr : rule.set_attrs)
        ci->attrs[attr.first] = attr.second;

    for (auto &param : rule.set_params)
        ci->params[param.first] = param.second;
}

void XilinxPacker::generic_xform(const std::unordered_map<IdString, XFormRule> &rules, bool print_summary)
{
    std::map<std::string, int> cell_count;
    std::map<std::string, int> new_types;
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (rules.count(ci->type)) {
            cell_count[ci->type.str(ctx)]++;
            xform_cell(rules, ci);
            new_types[ci->type.str(ctx)]++;
        }
    }
    if (print_summary) {
        for (auto &nt : new_types) {
            if(ctx->verbose){
                LogData logEntry = LogData::CreateLogStruct(
                    LevelCode::ALWAYS_LOG,
                    LogCategory::PACK,
                    PhaseType::PACK,
                    "Created cells"
                );
                log_always("    Created %d %s cells from:\n",logEntry ,nt.second, nt.first.c_str());
            }
            for (auto &cc : cell_count) {
                if (rules.at(ctx->id(cc.first)).new_type != ctx->id(nt.first))
                    continue;
                if(ctx->verbose){
                    LogData logEntry12 = LogData::CreateLogStruct(
                        LevelCode::ALWAYS_LOG,
                        LogCategory::PACK,
                        PhaseType::PACK,
                        "Created cells"
                    );
                    log_always("        %6dx %s\n",logEntry12, cc.second, cc.first.c_str());
                }
            }
        }
    }
}

std::unique_ptr<CellInfo> XilinxPacker::feed_through_lut(NetInfo *net, const std::vector<PortRef> &feed_users)
{
    std::unique_ptr<NetInfo> feedthru_net{new NetInfo};
    feedthru_net->name = ctx->id(net->name.str(ctx) + "$legal$" + std::to_string(++autoidx));
    std::unique_ptr<CellInfo> lut = create_lut(ctx, net->name.str(ctx) + "$LUT$" + std::to_string(++autoidx), {net},
                                               feedthru_net.get(), Property(2));

    for (auto &usr : feed_users) {
        disconnect_port(ctx, usr.cell, usr.port);
        connect_port(ctx, feedthru_net.get(), usr.cell, usr.port);
    }

    IdString netname = feedthru_net->name;
    ctx->nets[netname] = std::move(feedthru_net);
    return lut;
}

std::unique_ptr<CellInfo> XilinxPacker::feed_through_muxf(NetInfo *net, IdString type,
                                                          const std::vector<PortRef> &feed_users)
{
    std::unique_ptr<NetInfo> feedthru_net{new NetInfo};
    feedthru_net->name = ctx->id(net->name.str(ctx) + "$legal$" + std::to_string(++autoidx));
    std::unique_ptr<CellInfo> mux =
            create_cell(ctx, type, ctx->id(net->name.str(ctx) + "$MUX$" + std::to_string(++autoidx)));
    connect_port(ctx, net, mux.get(), ctx->id("I0"));
    connect_port(ctx, feedthru_net.get(), mux.get(), ctx->id("O"));
    connect_port(ctx, ctx->nets[ctx->id("$PACKER_GND_NET")].get(), mux.get(), ctx->id("S"));

    for (auto &usr : feed_users) {
        disconnect_port(ctx, usr.cell, usr.port);
        connect_port(ctx, feedthru_net.get(), usr.cell, usr.port);
    }

    IdString netname = feedthru_net->name;
    ctx->nets[netname] = std::move(feedthru_net);
    return mux;
}

IdString XilinxPacker::int_name(IdString base, const std::string &postfix, bool is_hierarchy)
{
    return ctx->id(base.str(ctx) + (is_hierarchy ? "$subcell$" : "$intcell$") + postfix);
}

NetInfo *XilinxPacker::create_internal_net(IdString base, const std::string &postfix, bool is_hierarchy)
{
    std::unique_ptr<NetInfo> net{new NetInfo};
    IdString name = ctx->id(base.str(ctx) + (is_hierarchy ? "$subnet$" : "$intnet$") + postfix);
    net->name = name;
    NPNR_ASSERT(!ctx->nets.count(name));
    ctx->nets[name] = std::move(net);
    return ctx->nets.at(name).get();
}

CellInfo *XilinxPacker::create_drom_lut(const std::string &name, CellInfo *base, std::vector<NetInfo *> address, NetInfo *dout, int z)
{
    std::unique_ptr<CellInfo> drom_lut = create_cell(ctx, ctx->id("SLICE_LUTX"), ctx->id(name));
    for (int i = 0; i < int(address.size()); i++)
        connect_port(ctx, address[i], drom_lut.get(), ctx->id("A" + std::to_string(i+1)));
    connect_port(ctx, dout, drom_lut.get(), ctx->id("O6"));

    drom_lut->constr_abs_z = true;
    drom_lut->constr_z = (z << 4) | BEL_6LUT;
    if (base != nullptr) {
        drom_lut->constr_parent = base;
        drom_lut->constr_x = 0;
        drom_lut->constr_y = 0;
        base->constr_children.push_back(drom_lut.get());
    }
    CellInfo *dl = drom_lut.get();
    new_cells.push_back(std::move(drom_lut));
    return dl;
}

void XilinxPacker::pack_rom()
{
    LogData logEntry5 = LogData::CreateLogStruct(
        LevelCode::INFO_LOG,
        LogCategory::PACK,
        PhaseType::PACK,
        "Packing ROM"
    );
    log_info("Packing ROM..\n",logEntry5);

    std::unordered_map<IdString, XFormRule> rom_rules;
    rom_rules[ctx->id("ROM64X1")].new_type = id_SLICE_LUTX;
    rom_rules[ctx->id("ROM64X1")].port_xform[ctx->id("O")] = ctx->id("O6");

    rom_rules[ctx->id("ROM32X1")].new_type = id_SLICE_LUTX;
    rom_rules[ctx->id("ROM32X1")].port_xform[ctx->id("O")] = ctx->id("O6");

    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == ctx->id("ROM64X1")) {
            for (int i=5;i>=0;i--){
                IdString old_name = ctx->id("A"+ std::to_string(i));
                IdString new_name = ctx->id("A"+ std::to_string(i + 1));
                rename_port(ctx, ci, old_name, new_name);
            }
            xform_cell(rom_rules,ci);
        } else if (ci->type == ctx->id("ROM32X1")) {
            for (int i = 4; i >= 0; i--) {
                IdString old_name = ctx->id("A" + std::to_string(i));
                IdString new_name = ctx->id("A" + std::to_string(i + 1));
                rename_port(ctx, ci, old_name, new_name);
            }
            // 对INIT值进行低32位补零操作
            auto init_property = get_or_default(ci->params, ctx->id("INIT"), Property(0));
            init_property.str.append(32 - init_property.str.size(), '0');
            init_property.str.insert(0, 32, '0');
            init_property.update_intval();
            ci->params[ctx->id("INIT")] = init_property;
            // 使用tie_port函数添加A6端口并连接到高电平
            tie_port(ci, "A6", true, false);
            xform_cell(rom_rules,ci);
        } else if (ci->type == ctx->id("ROM128X1")) {

            NetInfo *dout = get_net_or_empty(ci, ctx->id("O"));
            disconnect_port(ctx, ci, ctx->id("O"));

            std::vector<NetInfo *> addressw_low;
            for( auto i=0; i<=5; i++){
                addressw_low.push_back(ci->ports.at(ctx->id("A" + std::to_string(i))).net);
            }
            std::vector<NetInfo *> addressw_high;
            addressw_high.push_back(ci->ports.at(id_A6).net);
            // 创建一个vector来存储两个64位LUT的输出
            std::vector<NetInfo *> dout_interm;     

            // 获取INIT值并拆分
            auto init_property = get_or_default(ci->params, ctx->id("INIT"), Property(0, 128));
            Property init_low = init_property.extract(0, 64);
            Property init_high = init_property.extract(64, 64);

            NetInfo *dout_low = create_internal_net(ci->name, "O_LOW", false);
            auto base = create_drom_lut(ci->name.str(ctx)+ "/LOW", nullptr, addressw_low, dout_low, 3);
            base->attrs[ctx->id("X_ORIG_TYPE")] = Property("ROM128X1");
            // 设置低位的INIT值
            base->params[ctx->id("INIT")] = init_low;
            dout_interm.push_back(dout_low);

            NetInfo *dout_high = create_internal_net(ci->name, "O_HIGH", false);
            auto drom_high = create_drom_lut(ci->name.str(ctx)+ "/HIGH", base, addressw_low, dout_high, 2);
            drom_high->attrs[ctx->id("X_ORIG_TYPE")] = Property("ROM128X1");
            // 设置高位的INIT值
            drom_high->params[ctx->id("INIT")] = init_high;
            dout_interm.push_back(dout_high);

            create_muxf_tree(base, "O", dout_interm, addressw_high, dout, 2);
            packed_cells.insert(ci->name);
        } else if (ci->type == ctx->id("ROM256X1")) {
            NetInfo *dout = get_net_or_empty(ci, ctx->id("O"));
            disconnect_port(ctx, ci, ctx->id("O"));

            //低六位地址线
            std::vector<NetInfo *> addressw_low;
            for( auto i=0; i<=5; i++){
                addressw_low.push_back(ci->ports.at(ctx->id("A" + std::to_string(i))).net);
            }
            
            //高两位地址线
            std::vector<NetInfo *> addressw_high;
            addressw_high.push_back(ci->ports.at(ctx->id("A6")).net);
            addressw_high.push_back(ci->ports.at(ctx->id("A7")).net);

            std::vector<NetInfo *> dout_interm;

            auto init_property = get_or_default(ci->params, ctx->id("INIT"), Property(0, 256));
            Property init_a = init_property.extract(192, 64);
            Property init_b = init_property.extract(128, 64);
            Property init_c = init_property.extract(64, 64);
            Property init_d = init_property.extract(0, 64);           
            //D6LUT
            NetInfo *dout_d = create_internal_net(ci->name, "O_D", false);
            auto base = create_drom_lut(ci->name.str(ctx)+"/D", nullptr, addressw_low, dout_d, 3);
            base->attrs[ctx->id("X_ORIG_TYPE")] = Property("ROM256X1");
            base->params[ctx->id("INIT")] = init_d;
            dout_interm.push_back(dout_d);
            //C6LUT
            NetInfo *dout_c = create_internal_net(ci->name, "O_C", false);
            auto drom_c = create_drom_lut(ci->name.str(ctx)+"/C", base, addressw_low, dout_c, 2);
            drom_c->attrs[ctx->id("X_ORIG_TYPE")] = Property("ROM256X1");
            drom_c->params[ctx->id("INIT")] = init_c;
            dout_interm.push_back(dout_c);
            //B6LUT
            NetInfo *dout_b = create_internal_net(ci->name, "O_B", false);
            auto drom_b = create_drom_lut(ci->name.str(ctx)+"/B", base, addressw_low, dout_b, 1);
            drom_b->attrs[ctx->id("X_ORIG_TYPE")] = Property("ROM256X1");
            drom_b->params[ctx->id("INIT")] = init_b;
            dout_interm.push_back(dout_b);
            //A6LUT
            NetInfo *dout_a = create_internal_net(ci->name, "O_A", false);
            auto drom_a = create_drom_lut(ci->name.str(ctx)+"/A", base, addressw_low, dout_a, 0);
            drom_a->attrs[ctx->id("X_ORIG_TYPE")] = Property("ROM256X1");
            drom_a->params[ctx->id("INIT")] = init_a;
            dout_interm.push_back(dout_a);   

            create_muxf_tree(base, "O", dout_interm, addressw_high, dout, 0);
            packed_cells.insert(ci->name);            
        }
    }
    flush_cells();
}

void XilinxPacker::pack_luts()
{
    LogData logEntry7 = LogData::CreateLogStruct(
        LevelCode::INFO_LOG,
        LogCategory::PACK,
        PhaseType::PACK,
        "Packing LUTs"
    );
    log_info("Packing LUTs..\n",logEntry7);

    std::unordered_map<IdString, XFormRule> lut_rules;
    for (int k = 1; k <= 6; k++) {
        IdString lut = ctx->id("LUT" + std::to_string(k));
        lut_rules[lut].new_type = id_SLICE_LUTX;
        for (int i = 0; i < k; i++)
            lut_rules[lut].port_xform[ctx->id("I" + std::to_string(i))] = ctx->id("A" + std::to_string(i + 1));
        lut_rules[lut].port_xform[ctx->id("O")] = ctx->id("O6");
    }
    lut_rules[ctx->id("LUT6_2")] = lut_rules[ctx->id("LUT6")];
    generic_xform(lut_rules, true);
}

void XilinxPacker::pack_ffs()
{
    LogData logEntry9 = LogData::CreateLogStruct(
        LevelCode::INFO_LOG,
        LogCategory::PACK,
        PhaseType::PACK,
        "Packing flipflops"
    );
    log_info("Packing flipflops..\n",logEntry9);

    std::unordered_map<IdString, XFormRule> ff_rules;
    ff_rules[ctx->id("FDCE")].new_type = id_SLICE_FFX;
    ff_rules[ctx->id("FDCE")].port_xform[ctx->id("C")] = ctx->xc7 ? id_CK : id_CLK;
    ff_rules[ctx->id("FDCE")].port_xform[ctx->id("CLR")] = id_SR;
    ff_rules[ctx->id("FDCE")].set_attrs.emplace_back(ctx->id("CLK_STATUS"), Property("NOCLKINV"));
    // ff_rules[ctx->id("FDCE")].param_xform[ctx->id("IS_CLR_INVERTED")] = ctx->id("IS_SR_INVERTED");

    ff_rules[ctx->id("FDPE")].new_type = id_SLICE_FFX;
    ff_rules[ctx->id("FDPE")].port_xform[ctx->id("C")] = ctx->xc7 ? id_CK : id_CLK;
    ff_rules[ctx->id("FDPE")].port_xform[ctx->id("PRE")] = id_SR;
    ff_rules[ctx->id("FDPE")].set_attrs.emplace_back(ctx->id("CLK_STATUS"), Property("NOCLKINV"));
    // ff_rules[ctx->id("FDPE")].param_xform[ctx->id("IS_PRE_INVERTED")] = ctx->id("IS_SR_INVERTED");

    ff_rules[ctx->id("FDRE")].new_type = id_SLICE_FFX;
    ff_rules[ctx->id("FDRE")].port_xform[ctx->id("C")] = ctx->xc7 ? id_CK : id_CLK;
    ff_rules[ctx->id("FDRE")].port_xform[ctx->id("R")] = id_SR;
    ff_rules[ctx->id("FDRE")].set_attrs.emplace_back(ctx->id("X_FFSYNC"), Property(1));
    ff_rules[ctx->id("FDRE")].set_attrs.emplace_back(ctx->id("CLK_STATUS"), Property("NOCLKINV"));
    // ff_rules[ctx->id("FDRE")].param_xform[ctx->id("IS_R_INVERTED")] = ctx->id("IS_SR_INVERTED");

    ff_rules[ctx->id("FDSE")].new_type = id_SLICE_FFX;
    ff_rules[ctx->id("FDSE")].port_xform[ctx->id("C")] = ctx->xc7 ? id_CK : id_CLK;
    ff_rules[ctx->id("FDSE")].port_xform[ctx->id("S")] = id_SR;
    ff_rules[ctx->id("FDSE")].set_attrs.emplace_back(ctx->id("X_FFSYNC"), Property(1));
    ff_rules[ctx->id("FDSE")].set_attrs.emplace_back(ctx->id("CLK_STATUS"), Property("NOCLKINV"));
    // ff_rules[ctx->id("FDSE")].param_xform[ctx->id("IS_S_INVERTED")] = ctx->id("IS_SR_INVERTED");

    ff_rules[ctx->id("FDCE_1")] = ff_rules[ctx->id("FDCE")];
    ff_rules[ctx->id("FDCE_1")].set_params.emplace_back(ctx->id("IS_CLK_INVERTED"), 1);
    ff_rules[ctx->id("FDCE_1")].set_attrs.emplace_back(ctx->id("CLK_STATUS"), Property("CLKINV"));

    ff_rules[ctx->id("FDPE_1")] = ff_rules[ctx->id("FDPE")];
    ff_rules[ctx->id("FDPE_1")].set_params.emplace_back(ctx->id("IS_CLK_INVERTED"), 1);
    ff_rules[ctx->id("FDPE_1")].set_attrs.emplace_back(ctx->id("CLK_STATUS"), Property("CLKINV"));

    ff_rules[ctx->id("FDRE_1")] = ff_rules[ctx->id("FDRE")];
    ff_rules[ctx->id("FDRE_1")].set_params.emplace_back(ctx->id("IS_CLK_INVERTED"), 1);
    ff_rules[ctx->id("FDRE_1")].set_attrs.emplace_back(ctx->id("CLK_STATUS"), Property("CLKINV"));

    ff_rules[ctx->id("FDSE_1")] = ff_rules[ctx->id("FDSE")];
    ff_rules[ctx->id("FDSE_1")].set_params.emplace_back(ctx->id("IS_CLK_INVERTED"), 1);
    ff_rules[ctx->id("FDSE_1")].set_attrs.emplace_back(ctx->id("CLK_STATUS"), Property("CLKINV"));

    // 添加LATCH映射
    ff_rules[ctx->id("LDCE")].new_type = id_SLICE_FFX;
    ff_rules[ctx->id("LDCE")].port_xform[ctx->id("G")] = ctx->xc7 ? id_CK : id_CLK;
    ff_rules[ctx->id("LDCE")].port_xform[ctx->id("CLR")] = id_SR;
    ff_rules[ctx->id("LDCE")].port_xform[ctx->id("GE")] = id_CE;
    ff_rules[ctx->id("LDCE")].set_attrs.emplace_back(ctx->id("X_FF_AS_LATCH"), Property(1));
    ff_rules[ctx->id("LDCE")].set_attrs.emplace_back(ctx->id("CLK_STATUS"), Property("CLKINV"));
    ff_rules[ctx->id("LDCE")].set_params.emplace_back(ctx->id("IS_CLK_INVERTED"), 1);

    ff_rules[ctx->id("LDPE")].new_type = id_SLICE_FFX;
    ff_rules[ctx->id("LDPE")].port_xform[ctx->id("G")] = ctx->xc7 ? id_CK : id_CLK;
    ff_rules[ctx->id("LDPE")].port_xform[ctx->id("PRE")] = id_SR;
    ff_rules[ctx->id("LDPE")].port_xform[ctx->id("GE")] = id_CE;
    ff_rules[ctx->id("LDPE")].set_attrs.emplace_back(ctx->id("X_FF_AS_LATCH"), Property(1));
    ff_rules[ctx->id("LDPE")].set_attrs.emplace_back(ctx->id("CLK_STATUS"), Property("CLKINV"));
    ff_rules[ctx->id("LDPE")].set_params.emplace_back(ctx->id("IS_CLK_INVERTED"), 1);

    generic_xform(ff_rules, true);
}

void XilinxPacker::pack_lutffs()
{
    int pairs = 0;
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->constr_parent != nullptr || !ci->constr_children.empty())
            continue;
        if (ci->type != id_SLICE_FFX)
            continue;
        NetInfo *d = get_net_or_empty(ci, id_D);
        if (d->driver.cell == nullptr || d->driver.cell->type != id_SLICE_LUTX || (d->driver.port != id_O6 && d->driver.port != id_O5))
            continue;
        CellInfo *lut = d->driver.cell;
        if (lut->constr_parent != nullptr || !lut->constr_children.empty())
            continue;
        
        std::string ff_clk_status = str_or_default(ci->attrs, ctx->id("CLK_STATUS"), "NOCLKINV");
        std::string lut_clk_status = str_or_default(lut->attrs, ctx->id("CLK_STATUS"), "NONE");
        if (lut_clk_status != "NONE" && ff_clk_status != lut_clk_status){
            // 如果时钟信号反向不一致，无法作为chain
            continue;
        }
        // 如果ff有bel，无法作为chain
        std::string ff_bel = str_or_default(ci->attrs, ctx->id("BEL"), "NONE");
        if (ff_bel != "NONE"){
            continue;
        }
                
        lut->constr_children.push_back(ci);
        ci->constr_parent = lut;
        ci->constr_x = 0;
        ci->constr_y = 0;
        if(d->driver.port == id_O6){
            ci->constr_z = (BEL_FF - BEL_6LUT);
        }else{
            ci->constr_z = (BEL_FF2 - BEL_5LUT);
        }
        
        ++pairs;
    }
    if(ctx->verbose){
        LogData logEntry13 = LogData::CreateLogStruct(
            LevelCode::ALWAYS_LOG,
            LogCategory::PACK,
            PhaseType::PACK,
            "Constrained LUTFF pairs"
        );
        log_always("Constrained %d LUTFF pairs.\n", logEntry13,pairs);
    }
}

// pack阶段最后合法性检查
void XilinxPacker::check(){
    LogData logEntry11 = LogData::CreateLogStruct(
        LevelCode::INFO_LOG,
        LogCategory::PACK,
        PhaseType::PACK,
        "Packing Check"
    );
    log_info("Packing Check\n",logEntry11);
    // 检查chain中所有cell的clk状态是否一致
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (!ci->constr_children.empty() && (ci->type == id_CARRY4 || ci->type == id_SLICE_LUTX || ci->type == id_SLICE_FFX )) {
            std::string fix_clk_status = str_or_default(ci->attrs, id_CLK_STATUS, "NONE");
            // 遍历所有children，如果clk status不一致提示error
            for (auto child : ci->constr_children){
                auto child_clk_status = str_or_default(child->attrs, id_CLK_STATUS, "NONE");
                if (child_clk_status == "NONE")
                    continue;
                if (child_clk_status != fix_clk_status){
                    if (fix_clk_status == "NONE")
                        fix_clk_status = child_clk_status;
                    else 
                        log_error("chain clk status error");
                }
            }
            if (fix_clk_status != "NONE"){
                ci->attrs[id_CLK_STATUS] = Property(fix_clk_status);
            }
        }
    }
}

bool XilinxPacker::is_constrained(const CellInfo *cell)
{
    return cell->constr_x != cell->UNCONSTR || cell->constr_y != cell->UNCONSTR || cell->constr_z != cell->UNCONSTR;
}

void XilinxPacker::legalise_muxf_tree(CellInfo *curr, std::vector<CellInfo *> &mux_roots)
{
    if (curr->type.str(ctx).substr(0, 3) == "LUT")
        return;
    for (IdString p : {ctx->id("I0"), ctx->id("I1")}) {
        NetInfo *pn = get_net_or_empty(curr, p);
        if (pn == nullptr || pn->driver.cell == nullptr)
            continue;
        if (curr->type == ctx->id("MUXF7")) {
            if (pn->driver.cell->type.str(ctx).substr(0, 3) != "LUT" || is_constrained(pn->driver.cell)) {
                PortRef pr;
                pr.cell = curr;
                pr.port = p;
                auto i_feed = feed_through_lut(pn, {pr});
                new_cells.push_back(std::move(i_feed));
                continue;
            }
        } else {
            IdString next_type;
            if (curr->type == ctx->id("MUXF9"))
                next_type = ctx->id("MUXF8");
            else if (curr->type == ctx->id("MUXF8"))
                next_type = ctx->id("MUXF7");
            else
                NPNR_ASSERT_FALSE("bad mux type");
            if (pn->driver.cell->type != next_type || is_constrained(pn->driver.cell) ||
                bool_or_default(pn->driver.cell->attrs, ctx->id("MUX_TREE_ROOT"))) {
                PortRef pr;
                pr.cell = curr;
                pr.port = p;
                auto i_feed = feed_through_muxf(pn, next_type, {pr});
                legalise_muxf_tree(i_feed.get(), mux_roots);
                new_cells.push_back(std::move(i_feed));
                continue;
            }
        }
        legalise_muxf_tree(pn->driver.cell, mux_roots);
    }
}

void XilinxPacker::constrain_muxf_tree(CellInfo *curr, CellInfo *base, int zoffset)
{

    if (curr->type == id_SLICE_LUTX && (curr->constr_abs_z || curr->constr_parent != nullptr))
        return;

    int base_z = 0;
    if (base->type == ctx->id("MUXF7"))
        base_z = BEL_F7MUX;
    else if (base->type == ctx->id("MUXF8"))
        base_z = BEL_F8MUX;
    else if (base->type == ctx->id("MUXF9"))
        base_z = BEL_F9MUX;
    else if (base->constr_abs_z)
        base_z = base->constr_z;
    else
        NPNR_ASSERT_FALSE("unexpected mux base type");
    int curr_z = zoffset * 16;
    int input_spacing = 0;
    if (curr->type == ctx->id("MUXF7")) {
        curr_z += BEL_F7MUX;
        input_spacing = 1;
    } else if (curr->type == ctx->id("MUXF8")) {
        curr_z += BEL_F8MUX;
        input_spacing = 2;
    } else if (curr->type == ctx->id("MUXF9")) {
        curr_z += BEL_F9MUX;
        input_spacing = 4;
    } else
        curr_z += BEL_6LUT;
    if (curr != base) {
        curr->constr_x = 0;
        curr->constr_y = 0;
        curr->constr_z = curr_z - base_z;
        curr->constr_abs_z = false;
        curr->constr_parent = base;
        base->constr_children.push_back(curr);
    }
    if (curr->type == ctx->id("MUXF7") || curr->type == ctx->id("MUXF8") || curr->type == ctx->id("MUXF9")) {
        NetInfo *i0 = get_net_or_empty(curr, ctx->id("I0")), *i1 = get_net_or_empty(curr, ctx->id("I1"));
        if (i0 != nullptr && i0->driver.cell != nullptr)
            constrain_muxf_tree(i0->driver.cell, base, zoffset + input_spacing);
        if (i1 != nullptr && i1->driver.cell != nullptr)
            constrain_muxf_tree(i1->driver.cell, base, zoffset);
    }
}

void XilinxPacker::pack_muxfs()
{
    LogData logEntry4 = LogData::CreateLogStruct(
        LevelCode::INFO_LOG,
        LogCategory::PACK,
        PhaseType::PACK,
        "Packing MUX[789]s"
    );
    log_info("Packing MUX[789]s..\n",logEntry4);
    std::vector<CellInfo *> mux_roots;
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        ci->attrs.erase(ctx->id("MUX_TREE_ROOT"));
        if (ci->type == ctx->id("MUXF9")) {
            if (ctx->xc7)
                log_error("MUXF9 is not supported on xc7!\n");
            mux_roots.push_back(ci);
        } else if (ci->type == ctx->id("MUXF8")) {
            NetInfo *o = get_net_or_empty(ci, ctx->id("O"));
            if (o == nullptr || o->users.size() != 1 || o->users.at(0).cell->type != ctx->id("MUXF9") ||
                is_constrained(o->users.at(0).cell) || o->users.at(0).port == ctx->id("S"))
                mux_roots.push_back(ci);
        } else if (ci->type == ctx->id("MUXF7")) {
            NetInfo *o = get_net_or_empty(ci, ctx->id("O"));
            if (o == nullptr || o->users.size() != 1 || o->users.at(0).cell->type != ctx->id("MUXF8") ||
                is_constrained(o->users.at(0).cell) || o->users.at(0).port == ctx->id("S"))
                mux_roots.push_back(ci);
        }
    }
    for (auto root : mux_roots)
        root->attrs[ctx->id("MUX_TREE_ROOT")] = 1;
    for (auto root : mux_roots)
        legalise_muxf_tree(root, mux_roots);
    for (auto root : mux_roots)
        constrain_muxf_tree(root, root, 0);
}

void XilinxPacker::finalise_muxfs()
{
    std::unordered_map<IdString, XFormRule> muxf_rules;
    muxf_rules[ctx->id("MUXF9")].new_type = id_F9MUX;
    muxf_rules[ctx->id("MUXF9")].port_xform[ctx->id("I0")] = ctx->id("0");
    muxf_rules[ctx->id("MUXF9")].port_xform[ctx->id("I1")] = ctx->id("1");
    muxf_rules[ctx->id("MUXF9")].port_xform[ctx->id("S")] = ctx->id("S0");
    muxf_rules[ctx->id("MUXF9")].port_xform[ctx->id("O")] = ctx->id("OUT");
    muxf_rules[ctx->id("MUXF8")].new_type = ctx->xc7 ? ctx->id("SELMUX2_1") : id_F8MUX;
    muxf_rules[ctx->id("MUXF8")].port_xform = muxf_rules[ctx->id("MUXF9")].port_xform;
    muxf_rules[ctx->id("MUXF7")].new_type = ctx->xc7 ? ctx->id("SELMUX2_1") : id_F7MUX;
    muxf_rules[ctx->id("MUXF7")].port_xform = muxf_rules[ctx->id("MUXF9")].port_xform;
    generic_xform(muxf_rules, true);
}

void XilinxPacker::pack_srls()
{
    std::unordered_map<IdString, XFormRule> srl_rules;
    srl_rules[ctx->id("SRL16E")].new_type = id_SLICE_LUTX;
    srl_rules[ctx->id("SRL16E")].port_xform[ctx->id("CLK")] = id_CLK;
    srl_rules[ctx->id("SRL16E")].port_xform[ctx->id("CE")] = id_WE;
    srl_rules[ctx->id("SRL16E")].port_xform[ctx->id("D")] = id_DI1;
    srl_rules[ctx->id("SRL16E")].port_xform[ctx->id("Q")] = id_O5;
    srl_rules[ctx->id("SRL16E")].set_attrs.emplace_back(ctx->id("X_LUT_AS_SRL"), Property(1));
    srl_rules[ctx->id("SRL16E")].set_attrs.emplace_back(ctx->id("X_IS_SLICEM"), Property(1));

    srl_rules[ctx->id("SRLC32E")].new_type = id_SLICE_LUTX;
    srl_rules[ctx->id("SRLC32E")].port_xform[ctx->id("CLK")] = id_CLK;
    srl_rules[ctx->id("SRLC32E")].port_xform[ctx->id("CE")] = id_WE;
    srl_rules[ctx->id("SRLC32E")].port_xform[ctx->id("D")] = id_DI1;
    srl_rules[ctx->id("SRLC32E")].port_xform[ctx->id("Q")] = id_O6;
    srl_rules[ctx->id("SRLC32E")].set_attrs.emplace_back(ctx->id("X_LUT_AS_SRL"), Property(1));
    srl_rules[ctx->id("SRLC32E")].set_attrs.emplace_back(ctx->id("X_IS_SLICEM"), Property(1));

    // CFGLUT5
    srl_rules[ctx->id("CFGLUT5")].new_type = id_SLICE_LUTX;
    srl_rules[ctx->id("CFGLUT5")].port_xform[ctx->id("CLK")] = id_CLK;
    srl_rules[ctx->id("CFGLUT5")].port_xform[ctx->id("CE")] = id_WE;
    srl_rules[ctx->id("CFGLUT5")].port_xform[ctx->id("CDI")] = id_DI1;
    srl_rules[ctx->id("CFGLUT5")].port_xform[ctx->id("O6")] = id_O6;
    srl_rules[ctx->id("CFGLUT5")].port_xform[ctx->id("O5")] = id_O5;
    srl_rules[ctx->id("CFGLUT5")].port_xform[ctx->id("I0")] = id_A2;
    srl_rules[ctx->id("CFGLUT5")].port_xform[ctx->id("I1")] = id_A3;
    srl_rules[ctx->id("CFGLUT5")].port_xform[ctx->id("I2")] = id_A4;
    srl_rules[ctx->id("CFGLUT5")].port_xform[ctx->id("I3")] = id_A5;
    srl_rules[ctx->id("CFGLUT5")].port_xform[ctx->id("I4")] = id_A6;
    srl_rules[ctx->id("CFGLUT5")].set_attrs.emplace_back(ctx->id("X_LUT_AS_SRL"), Property(1));
    srl_rules[ctx->id("CFGLUT5")].set_attrs.emplace_back(ctx->id("X_IS_SLICEM"), Property(1));

    // FIXME: Q31 support
    generic_xform(srl_rules, true);
    // Fixup SRL inputs
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type != id_SLICE_LUTX)
            continue;
        std::string orig_type = str_or_default(ci->attrs, ctx->id("X_ORIG_TYPE"), "");
        if (orig_type == "SRL16E") {
            for (int i = 3; i >= 0; i--) {
                rename_port(ctx, ci, ctx->id("A" + std::to_string(i)), ctx->id("A" + std::to_string(i + 2)));
            }
            for (auto tp : {id_A1}) {
                ci->ports[tp].name = tp;
                ci->ports[tp].type = PORT_IN;
                connect_port(ctx, ctx->nets[ctx->id("$PACKER_VCC_NET")].get(), ci, tp);
            }
            // 修改params INIT
            auto init_it = ci->params.find(ctx->id("INIT"));
            if(init_it != ci->params.end()){
                std::string ts(init_it->second.str);
                // init_it->second.str.reserve(32);
                init_it->second.str.clear();
                for(auto i=0; i<ts.size(); i++){
                    init_it->second.str.push_back(ts[i]);
                    init_it->second.str.push_back(ts[i]);
                }
                for(auto pre_index=init_it->second.str.size(); pre_index < 32; pre_index++){
                    init_it->second.str.push_back('0');
                }
                init_it->second.update_intval();
            }
            
        } else if (orig_type == "SRLC32E") {
            for (int i = 4; i >= 0; i--) {
                rename_port(ctx, ci, ctx->id("A" + std::to_string(i)), ctx->id("A" + std::to_string(i + 2)));
            }
            for (auto tp : {id_A1}) {
                ci->ports[tp].name = tp;
                ci->ports[tp].type = PORT_IN;
                connect_port(ctx, ctx->nets[ctx->id("$PACKER_VCC_NET")].get(), ci, tp);
            }
            // 修改params INIT
            auto init_it = ci->params.find(ctx->id("INIT"));
            if(init_it != ci->params.end()){
                std::string ts(init_it->second.str);
                init_it->second.str.clear();
                for(auto i=0; i<ts.size(); i++){
                    init_it->second.str.push_back(ts[i]);
                    init_it->second.str.push_back(ts[i]);
                }
                for(auto pre_index=init_it->second.str.size(); pre_index < 64; pre_index++){
                    init_it->second.str.push_back('0');
                }
                init_it->second.update_intval();
            }
        } else if(orig_type == "CFGLUT5"){
            ci->ports[id_A1].name = id_A1;
            ci->ports[id_A1].type = PORT_IN;
            connect_port(ctx, ctx->nets[ctx->id("$PACKER_VCC_NET")].get(), ci, id_A1);

            // 修改params INIT
            auto init_it = ci->params.find(ctx->id("INIT"));
            if(init_it != ci->params.end()){
                std::string ts(init_it->second.str);
                init_it->second.str.clear();
                for(auto i=0; i<ts.size(); i++){
                    init_it->second.str.push_back(ts[i]);
                    init_it->second.str.push_back(ts[i]);
                }
                for(auto pre_index=init_it->second.str.size(); pre_index < 64; pre_index++){
                    init_it->second.str.push_back('0');
                }
                init_it->second.update_intval();
            }
        }
    }
}

void XilinxPacker::pack_constants()
{
    LogData logEntry1 = LogData::CreateLogStruct(
        LevelCode::INFO_LOG,
        LogCategory::PACK,
        PhaseType::PACK,
        "Packing constants.."
    );
    log_info("Packing constants..\n",logEntry1);

    if (tied_pins.empty())
        get_tied_pins(ctx, tied_pins);
    if (invertible_pins.empty())
        get_invertible_pins(ctx, invertible_pins);
    if (!ctx->cells.count(ctx->id("$PACKER_GND_DRV"))) {
        std::unique_ptr<CellInfo> gnd_cell{new CellInfo};
        gnd_cell->name = ctx->id("$PACKER_GND_DRV");
        gnd_cell->type = id_PSEUDO_GND;
        gnd_cell->ports[id_Y].name = id_Y;
        gnd_cell->ports[id_Y].type = PORT_OUT;
        std::unique_ptr<NetInfo> gnd_net = std::unique_ptr<NetInfo>(new NetInfo);
        gnd_net->name = ctx->id("$PACKER_GND_NET");
        gnd_net->driver.cell = gnd_cell.get();
        gnd_net->driver.port = id_Y;
        gnd_cell->ports.at(id_Y).net = gnd_net.get();

        std::unique_ptr<CellInfo> vcc_cell{new CellInfo};
        vcc_cell->name = ctx->id("$PACKER_VCC_DRV");
        vcc_cell->type = id_PSEUDO_VCC;
        vcc_cell->ports[id_Y].name = id_Y;
        vcc_cell->ports[id_Y].type = PORT_OUT;
        std::unique_ptr<NetInfo> vcc_net = std::unique_ptr<NetInfo>(new NetInfo);
        vcc_net->name = ctx->id("$PACKER_VCC_NET");
        vcc_net->driver.cell = vcc_cell.get();
        vcc_net->driver.port = id_Y;
        vcc_cell->ports.at(id_Y).net = vcc_net.get();

        ctx->cells[gnd_cell->name] = std::move(gnd_cell);
        ctx->nets[gnd_net->name] = std::move(gnd_net);
        ctx->cells[vcc_cell->name] = std::move(vcc_cell);
        ctx->nets[vcc_net->name] = std::move(vcc_net);
    }
    NetInfo *gnd = ctx->nets[ctx->id("$PACKER_GND_NET")].get(), *vcc = ctx->nets[ctx->id("$PACKER_VCC_NET")].get();

    std::vector<IdString> dead_nets;

    std::vector<std::tuple<CellInfo *, IdString, bool>> const_ports;

    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (!tied_pins.count(ci->type))
            continue;
        auto &tp = tied_pins.at(ci->type);
        for (auto port : tp) {
            if (cell.second->ports.count(port.first) && cell.second->ports.at(port.first).net != nullptr &&
                cell.second->ports.at(port.first).net->driver.cell != nullptr)
                continue;
            const_ports.emplace_back(ci, port.first, port.second);
        }
    }

    for (auto net : sorted(ctx->nets)) {
        NetInfo *ni = net.second;
        if (ni->driver.cell != nullptr && ni->driver.cell->type == ctx->id("GND")) {
            IdString drv_cell = ni->driver.cell->name;
            for (auto &usr : ni->users) {
                const_ports.emplace_back(usr.cell, usr.port, false);
                usr.cell->ports.at(usr.port).net = nullptr;
            }
            dead_nets.push_back(net.first);
            ctx->cells.erase(drv_cell);
        } else if (ni->driver.cell != nullptr && ni->driver.cell->type == ctx->id("VCC")) {
            IdString drv_cell = ni->driver.cell->name;
            for (auto &usr : ni->users) {
                const_ports.emplace_back(usr.cell, usr.port, true);
                usr.cell->ports.at(usr.port).net = nullptr;
            }
            dead_nets.push_back(net.first);
            ctx->cells.erase(drv_cell);
        }
    }

    for (auto port : const_ports) {
        CellInfo *ci;
        IdString pname;
        bool cval;
        std::tie(ci, pname, cval) = port;

        if (!ci->ports.count(pname)) {
            ci->ports[pname].name = pname;
            ci->ports[pname].type = PORT_IN;
        }
        if (ci->ports.at(pname).net != nullptr) {
            // Case where a port with a default tie value is previously connected to an undriven net
            NPNR_ASSERT(ci->ports.at(pname).net->driver.cell == nullptr);
            disconnect_port(ctx, ci, pname);
        }

        // if (!cval && invertible_pins.count(ci->type) && invertible_pins.at(ci->type).count(pname)) {
        //     // Invertible pins connected to zero are optimised to a connection to Vcc (which is easier to route)
        //     // and an inversion
        //     ci->params[ctx->id("IS_" + pname.str(ctx) + "_INVERTED")] = Property(1);
        //     cval = true;
        // }

        connect_port(ctx, cval ? vcc : gnd, ci, pname);
    }

    for (auto dn : dead_nets) {
        ctx->nets.erase(dn);
    }
}

void XilinxPacker::constrains_bel_loc()
{
    if(!ctx->constrains.empty()) {
         for (auto cell : sorted(ctx->cells)) {
            // 如果constrains的key里有cell的name
            if (ctx->constrains.count(cell.first)) {
                auto cons = ctx->constrains.at(cell.first);
                if (!cons.empty()) {
                    auto bel_type = str_or_default(cons, ctx->id("BEL_TYPE"), "");
                    auto loc = str_or_default(cons, ctx->id("LOC"), "");
                    if(!loc.empty() && !bel_type.empty())
                        cell.second->attrs[ctx->id("BEL")] = loc + "/" + bel_type;
                }
                // 使用过的约束需要删除
                ctx->constrains.erase(cell.first);
            }
        }
    }
}

void XilinxPacker::rename_net(IdString old, IdString newname)
{
    std::unique_ptr<NetInfo> ni;
    std::swap(ni, ctx->nets[old]);
    ctx->nets.erase(old);
    ni->name = newname;
    ctx->nets[newname] = std::move(ni);
}

void XilinxPacker::tie_port(CellInfo *ci, const std::string &port, bool value, bool inv)
{
    IdString p = ctx->id(port);
    if (!ci->ports.count(p)) {
        ci->ports[p].name = p;
        ci->ports[p].type = PORT_IN;
    }
    if (value || inv)
        connect_port(ctx, ctx->nets.at(ctx->id("$PACKER_VCC_NET")).get(), ci, p);
    else
        connect_port(ctx, ctx->nets.at(ctx->id("$PACKER_GND_NET")).get(), ci, p);
    if (!value && inv)
        ci->params[ctx->id("IS_" + port + "_INVERTED")] = Property(1);
}

void USPacker::pack_bram()
{
    log_info("Packing BRAM..\n");

    // Rules for normal TDP BRAM
    std::unordered_map<IdString, XFormRule> bram_rules;
    bram_rules[ctx->id("RAMB18E2")].new_type = id_RAMB18E2_RAMB18E2;
    bram_rules[ctx->id("RAMB18E2")].port_multixform[ctx->id(std::string("WEA[0]"))] = {ctx->id("WEA0"),
                                                                                       ctx->id("WEA1")};
    bram_rules[ctx->id("RAMB18E2")].port_multixform[ctx->id(std::string("WEA[1]"))] = {ctx->id("WEA2"),
                                                                                       ctx->id("WEA3")};
    bram_rules[ctx->id("RAMB36E2")].new_type = id_RAMB36E2_RAMB36E2;

    // Some ports have upper/lower bel pins in 36-bit mode
    std::vector<std::pair<IdString, std::vector<std::string>>> ul_pins;
    get_bram36_ul_pins(ctx, ul_pins);
    for (auto &ul : ul_pins) {
        for (auto &bp : ul.second)
            bram_rules[ctx->id("RAMB36E2")].port_multixform[ul.first].push_back(ctx->id(bp));
    }
    bram_rules[ctx->id("RAMB36E2")].port_multixform[ctx->id("ECCPIPECE")] = {ctx->id("ECCPIPECEL")};

    // Special rules for SDP rules, relating to WE connectivity
    std::unordered_map<IdString, XFormRule> sdp_bram_rules = bram_rules;
    for (int i = 0; i < 2; i++) {
        // Connects to two WEBWE bel pins
        sdp_bram_rules[ctx->id("RAMB18E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))]
                .push_back(ctx->id("WEBWE" + std::to_string(i * 2)));
        sdp_bram_rules[ctx->id("RAMB18E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))]
                .push_back(ctx->id("WEBWE" + std::to_string(i * 2 + 1)));
        // Not used in SDP mode
        sdp_bram_rules[ctx->id("RAMB18E2")]
                .port_multixform[ctx->id(std::string("WEA[" + std::to_string(i) + "]"))] = {};
    }
    for (int i = 0; i < 2; i++) {
        // Connects to two WEA bel pins
        sdp_bram_rules[ctx->id("RAMB18E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i + 2) + "]"))]
                .push_back(ctx->id("WEA" + std::to_string(i * 2)));
        sdp_bram_rules[ctx->id("RAMB18E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i + 2) + "]"))]
                .push_back(ctx->id("WEA" + std::to_string(i * 2 + 1)));
    }

    for (int i = 0; i < 4; i++) {
        sdp_bram_rules[ctx->id("RAMB36E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))]
                .clear();
        sdp_bram_rules[ctx->id("RAMB36E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i + 4) + "]"))]
                .clear();
        // Connects to two WEBWE bel pins
        sdp_bram_rules[ctx->id("RAMB36E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))]
                .push_back(ctx->id("WEBWEL" + std::to_string(i)));
        sdp_bram_rules[ctx->id("RAMB36E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))]
                .push_back(ctx->id("WEBWEU" + std::to_string(i)));
        sdp_bram_rules[ctx->id("RAMB36E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i + 4) + "]"))]
                .push_back(ctx->id("WEAL" + std::to_string(i)));
        sdp_bram_rules[ctx->id("RAMB36E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i + 4) + "]"))]
                .push_back(ctx->id("WEAU" + std::to_string(i)));
        // Not used in SDP mode
        sdp_bram_rules[ctx->id("RAMB36E2")]
                .port_multixform[ctx->id(std::string("WEA[" + std::to_string(i) + "]"))] = {};
    }

    // 72-bit BRAMs: drop upper bits of WEB in TDP mode
    for (int i = 4; i < 8; i++)
        bram_rules[ctx->id("RAMB36E2")].port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))] = {};

    // Process SDP BRAM first
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if ((ci->type == ctx->id("RAMB18E2") &&
             int_or_default(ci->params, ctx->id(std::string("WRITE_WIDTH_B")), 0) == 36) ||
            (ci->type == ctx->id("RAMB36E2") &&
             int_or_default(ci->params, ctx->id(std::string("WRITE_WIDTH_B")), 0) == 72))
            xform_cell(sdp_bram_rules, ci);
    }

    // Rewrite byte enables according to data width
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == ctx->id("RAMB18E2") || ci->type == ctx->id("RAMB36E2")) {
            for (char port : {'A', 'B'}) {
                int write_width = int_or_default(ci->params, ctx->id(std::string("WRITE_WIDTH_") + port), 18);
                int we_width;
                if (ci->type == ctx->id("RAMB36E2"))
                    we_width = 4;
                else
                    we_width = (port == 'B') ? 4 : 2;
                if (write_width >= (9 * we_width))
                    continue;
                int used_we_width = std::max(write_width / 9, 1);
                for (int i = used_we_width; i < we_width; i++) {
                    NetInfo *low_we = get_net_or_empty(ci, ctx->id(std::string(port == 'B' ? "WEBWE[" : "WEA[") +
                                                                   std::to_string(i % used_we_width) + "]"));
                    IdString curr_we = ctx->id(std::string(port == 'B' ? "WEBWE[" : "WEA[") + std::to_string(i) + "]");
                    if (!ci->ports.count(curr_we)) {
                        ci->ports[curr_we].type = PORT_IN;
                        ci->ports[curr_we].name = curr_we;
                    }
                    disconnect_port(ctx, ci, curr_we);
                    connect_port(ctx, low_we, ci, curr_we);
                }
            }
        }
    }

    generic_xform(bram_rules, false);

    // These pins have no logical mapping, so must be tied after transformation
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == id_RAMB18E2_RAMB18E2) {
            for (int i = 2; i < 4; i++) {
                IdString port = ctx->id("WEA" + std::to_string(i));
                if (!ci->ports.count(port)) {
                    ci->ports[port].name = port;
                    ci->ports[port].type = PORT_IN;
                    connect_port(ctx, ctx->nets[ctx->id("$PACKER_VCC_NET")].get(), ci, port);
                }
            }
        }
    }
}

void XC7Packer::pack_bram()
{
    LogData logEntry6 = LogData::CreateLogStruct(
        LevelCode::INFO_LOG,
        LogCategory::PACK,
        PhaseType::PACK,
        "Packing BRAM"
    );
    log_info("Packing BRAM..\n",logEntry6);

    // Rules for normal TDP BRAM
    std::unordered_map<IdString, XFormRule> bram_rules;
    bram_rules[ctx->id("RAMB18E1")].new_type = id_RAMB18E1_RAMB18E1;
    bram_rules[ctx->id("RAMB18E1")].port_multixform[ctx->id(std::string("WEA[0]"))] = {ctx->id("WEA0"),
                                                                                       ctx->id("WEA1")};
    bram_rules[ctx->id("RAMB18E1")].port_multixform[ctx->id(std::string("WEA[1]"))] = {ctx->id("WEA2"),
                                                                                       ctx->id("WEA3")};
    bram_rules[ctx->id("RAMB36E1")].new_type = id_RAMB36E1_RAMB36E1;

    // Some ports have upper/lower bel pins in 36-bit mode
    std::vector<std::pair<IdString, std::vector<std::string>>> ul_pins;
    get_bram36_ul_pins(ctx, ul_pins);
    for (auto &ul : ul_pins) {
        for (auto &bp : ul.second)
            bram_rules[ctx->id("RAMB36E1")].port_multixform[ul.first].push_back(ctx->id(bp));
    }
    bram_rules[ctx->id("RAMB36E1")].port_multixform[ctx->id("ADDRARDADDR[15]")].push_back(ctx->id("ADDRARDADDRL15"));
    bram_rules[ctx->id("RAMB36E1")].port_multixform[ctx->id("ADDRBWRADDR[15]")].push_back(ctx->id("ADDRBWRADDRL15"));

    // Special rules for SDP rules, relating to WE connectivity
    std::unordered_map<IdString, XFormRule> sdp_bram_rules = bram_rules;
    for (int i = 0; i < 4; i++) {
        // Connects to two WEBWE bel pins
        sdp_bram_rules[ctx->id("RAMB18E1")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))]
                .push_back(ctx->id("WEBWE" + std::to_string(i * 2)));
        sdp_bram_rules[ctx->id("RAMB18E1")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))]
                .push_back(ctx->id("WEBWE" + std::to_string(i * 2 + 1)));
        // Not used in SDP mode
        sdp_bram_rules[ctx->id("RAMB18E1")]
                .port_multixform[ctx->id(std::string("WEA[" + std::to_string(i) + "]"))] = {};
    }

    for (int i = 0; i < 8; i++) {
        sdp_bram_rules[ctx->id("RAMB36E1")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))]
                .clear();
        // Connects to two WEBWE bel pins
        sdp_bram_rules[ctx->id("RAMB36E1")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))]
                .push_back(ctx->id("WEBWEL" + std::to_string(i)));
        sdp_bram_rules[ctx->id("RAMB36E1")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))]
                .push_back(ctx->id("WEBWEU" + std::to_string(i)));
        // Not used in SDP mode
        sdp_bram_rules[ctx->id("RAMB36E1")]
                .port_multixform[ctx->id(std::string("WEA[" + std::to_string(i) + "]"))] = {};
    }

    // 72-bit BRAMs: drop upper bits of WEB in TDP mode
    for (int i = 4; i < 8; i++)
        bram_rules[ctx->id("RAMB36E1")].port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))] = {};

    // Process SDP BRAM first
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if ((ci->type == ctx->id("RAMB18E1") &&
             int_or_default(ci->params, ctx->id(std::string("WRITE_WIDTH_B")), 0) == 36) ||
            (ci->type == ctx->id("RAMB36E1") &&
             int_or_default(ci->params, ctx->id(std::string("WRITE_WIDTH_B")), 0) == 72))
            xform_cell(sdp_bram_rules, ci);
    }

     // fifo
    std::unordered_map<IdString, XFormRule> fifo_normal_rules, fifo_max_rules;//fifo18_36和fifo_36_72使用fifo_max_rules
    fifo_normal_rules[ctx->id("FIFO18E1")].new_type = id_FIFO18E1_FIFO18E1;
    fifo_normal_rules[ctx->id("FIFO36E1")].new_type = id_FIFO36E1_FIFO36E1;
    fifo_max_rules[ctx->id("FIFO18E1")].new_type = id_FIFO18E1_FIFO18E1;
    fifo_max_rules[ctx->id("FIFO36E1")].new_type = id_FIFO36E1_FIFO36E1;
    // fifo18
    for(int i=0; i<32; i++){
        if(i<16){
            fifo_normal_rules[ctx->id("FIFO18E1")].port_xform[ctx->id(std::string("DI[" + std::to_string(i) + "]"))] = ctx->id("DIBDI" + std::to_string(i));
            fifo_max_rules[ctx->id("FIFO18E1")].port_xform[ctx->id(std::string("DI[" + std::to_string(i) + "]"))] = ctx->id("DIADI" + std::to_string(i));
        }else{
            fifo_normal_rules[ctx->id("FIFO18E1")].port_xform[ctx->id(std::string("DI[" + std::to_string(i) + "]"))] = ctx->id("DIADI" + std::to_string(i-16));
            fifo_max_rules[ctx->id("FIFO18E1")].port_xform[ctx->id(std::string("DI[" + std::to_string(i) + "]"))] = ctx->id("DIBDI" + std::to_string(i-16));
        }
    }
    for(int i=0; i<4; i++){
        if(i<2){
            fifo_normal_rules[ctx->id("FIFO18E1")].port_xform[ctx->id(std::string("DIP[" + std::to_string(i) + "]"))] = ctx->id("DIPBDIP" + std::to_string(i));
            fifo_max_rules[ctx->id("FIFO18E1")].port_xform[ctx->id(std::string("DIP[" + std::to_string(i) + "]"))] = ctx->id("DIPADIP" + std::to_string(i));
        } else {
            fifo_normal_rules[ctx->id("FIFO18E1")].port_xform[ctx->id(std::string("DIP[" + std::to_string(i) + "]"))] = ctx->id("DIPADIP" + std::to_string(i-2));
            fifo_max_rules[ctx->id("FIFO18E1")].port_xform[ctx->id(std::string("DIP[" + std::to_string(i) + "]"))] = ctx->id("DIPBDIP" + std::to_string(i-2));
        }
    }
    fifo_normal_rules[ctx->id("FIFO18E1")].port_multixform[ctx->id(std::string("RDCLK"))] = {ctx->id("RDCLK"),ctx->id("RDRCLK")};
    fifo_max_rules[ctx->id("FIFO18E1")].port_multixform[ctx->id(std::string("RDCLK"))] = {ctx->id("RDCLK"),ctx->id("RDRCLK")};
    // fifo36
    for(int i=0; i<64; i++){
        if(i<32){
            fifo_normal_rules[ctx->id("FIFO36E1")].port_xform[ctx->id(std::string("DI[" + std::to_string(i) + "]"))] = ctx->id("DIBDI" + std::to_string(i));
            fifo_max_rules[ctx->id("FIFO36E1")].port_xform[ctx->id(std::string("DI[" + std::to_string(i) + "]"))] = ctx->id("DIADI" + std::to_string(i));
        }else{
            fifo_normal_rules[ctx->id("FIFO36E1")].port_xform[ctx->id(std::string("DI[" + std::to_string(i) + "]"))] = ctx->id("DIADI" + std::to_string(i-32));
            fifo_max_rules[ctx->id("FIFO36E1")].port_xform[ctx->id(std::string("DI[" + std::to_string(i) + "]"))] = ctx->id("DIBDI" + std::to_string(i-32));
        }
    }
    for(int i=0; i<8; i++){
        if(i<4){
            fifo_normal_rules[ctx->id("FIFO36E1")].port_xform[ctx->id(std::string("DIP[" + std::to_string(i) + "]"))] = ctx->id("DIPBDIP" + std::to_string(i));
            fifo_max_rules[ctx->id("FIFO36E1")].port_xform[ctx->id(std::string("DIP[" + std::to_string(i) + "]"))] = ctx->id("DIPADIP" + std::to_string(i));
        } else {
            fifo_normal_rules[ctx->id("FIFO36E1")].port_xform[ctx->id(std::string("DIP[" + std::to_string(i) + "]"))] = ctx->id("DIPADIP" + std::to_string(i-4));
            fifo_max_rules[ctx->id("FIFO36E1")].port_xform[ctx->id(std::string("DIP[" + std::to_string(i) + "]"))] = ctx->id("DIPBDIP" + std::to_string(i-4));
        }
    }
    fifo_normal_rules[ctx->id("FIFO36E1")].port_multixform[ctx->id(std::string("RDCLK"))] = {ctx->id("RDCLKU"),ctx->id("RDCLKL"),ctx->id("RDRCLKU"),ctx->id("RDRCLKL")};
    fifo_normal_rules[ctx->id("FIFO36E1")].port_multixform[ctx->id(std::string("WRCLK"))] = {ctx->id("WRCLKU"),ctx->id("WRCLKL")};
    fifo_normal_rules[ctx->id("FIFO36E1")].port_multixform[ctx->id(std::string("RDEN"))] = {ctx->id("RDENU"),ctx->id("RDENL")};
    fifo_normal_rules[ctx->id("FIFO36E1")].port_multixform[ctx->id(std::string("WREN"))] = {ctx->id("WRENU"),ctx->id("WRENL")};
    fifo_normal_rules[ctx->id("FIFO36E1")].port_multixform[ctx->id(std::string("REGCE"))] = {ctx->id("REGCEU"),ctx->id("REGCEL")};
    fifo_normal_rules[ctx->id("FIFO36E1")].port_multixform[ctx->id(std::string("RSTREG"))] = {ctx->id("RSTREGU"),ctx->id("RSTREGL")};
    fifo_max_rules[ctx->id("FIFO36E1")].port_multixform = fifo_normal_rules[ctx->id("FIFO36E1")].port_multixform;


    // fifo映射
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if(ci->type == ctx->id("FIFO18E1")){
            fold_inverter(ci, "RST");
            std::string fifo_mode = str_or_default(ci->params, ctx->id("FIFO_MODE"),"FIFO18");
            if(fifo_mode == "FIFO18"){
                xform_cell(fifo_normal_rules, ci);
            } else{
                // FIFO18_36
                NPNR_ASSERT(fifo_mode == "FIFO18_36");
                xform_cell(fifo_max_rules, ci);
            }
        } else if(ci->type == ctx->id("FIFO36E1")){
            fold_inverter(ci, "RST");
            std::string fifo_mode = str_or_default(ci->params, ctx->id("FIFO_MODE"),"FIFO36");
            if(fifo_mode == "FIFO36"){
                xform_cell(fifo_normal_rules, ci);
            } else {
                NPNR_ASSERT(fifo_mode == "FIFO36_72");
                xform_cell(fifo_max_rules, ci);
            }
        }
    }

    // Rewrite byte enables according to data width
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == ctx->id("RAMB18E1") || ci->type == ctx->id("RAMB36E1")) {
            for (char port : {'A', 'B'}) {
                int write_width = int_or_default(ci->params, ctx->id(std::string("WRITE_WIDTH_") + port), 18);
                int we_width;
                if (ci->type == ctx->id("RAMB36E1"))
                    we_width = 4;
                else
                    we_width = (port == 'B') ? 4 : 2;
                if (write_width >= (9 * we_width))
                    continue;
                int used_we_width = std::max(write_width / 9, 1);
                for (int i = used_we_width; i < we_width; i++) {
                    NetInfo *low_we = get_net_or_empty(ci, ctx->id(std::string(port == 'B' ? "WEBWE[" : "WEA[") +
                                                                   std::to_string(i % used_we_width) + "]"));
                    IdString curr_we = ctx->id(std::string(port == 'B' ? "WEBWE[" : "WEA[") + std::to_string(i) + "]");
                    if (!ci->ports.count(curr_we)) {
                        ci->ports[curr_we].type = PORT_IN;
                        ci->ports[curr_we].name = curr_we;
                    }
                    disconnect_port(ctx, ci, curr_we);
                    connect_port(ctx, low_we, ci, curr_we);
                }
            }
        }
    }

    generic_xform(bram_rules, false);

    // These pins have no logical mapping, so must be tied after transformation
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == id_RAMB18E1_RAMB18E1) {
            int wwa = int_or_default(ci->params, ctx->id("WRITE_WIDTH_A"), 0);
            for (int i = ((wwa == 0) ? 0 : 2); i < 4; i++) {
                IdString port = ctx->id("WEA" + std::to_string(i));
                if (!ci->ports.count(port)) {
                    ci->ports[port].name = port;
                    ci->ports[port].type = PORT_IN;
                    connect_port(ctx, ctx->nets[ctx->id("$PACKER_GND_NET")].get(), ci, port);
                }
            }
            int wwb = int_or_default(ci->params, ctx->id("WRITE_WIDTH_B"), 0);
            if (wwb != 36) {
                for (int i = 4; i < 8; i++) {
                    IdString port = ctx->id("WEBWE" + std::to_string(i));
                    if (!ci->ports.count(port)) {
                        ci->ports[port].name = port;
                        ci->ports[port].type = PORT_IN;
                        connect_port(ctx, ctx->nets[ctx->id("$PACKER_GND_NET")].get(), ci, port);
                    }
                }
            }
            for (auto p : {ctx->id("ADDRATIEHIGH0"), ctx->id("ADDRATIEHIGH1"), ctx->id("ADDRBTIEHIGH0"),
                           ctx->id("ADDRBTIEHIGH1")}) {
                if (!ci->ports.count(p)) {
                    ci->ports[p].name = p;
                    ci->ports[p].type = PORT_IN;
                } else {
                    disconnect_port(ctx, ci, p);
                }
                connect_port(ctx, ctx->nets[ctx->id("$PACKER_VCC_NET")].get(), ci, p);
            }
        } else if (ci->type == id_RAMB36E1_RAMB36E1) {
            for (auto p : {ctx->id("ADDRARDADDRL15"), ctx->id("ADDRBWRADDRL15")}) {
                if (!ci->ports.count(p)) {
                    ci->ports[p].name = p;
                    ci->ports[p].type = PORT_IN;
                } else {
                    disconnect_port(ctx, ci, p);
                }
                connect_port(ctx, ctx->nets[ctx->id("$PACKER_VCC_NET")].get(), ci, p);
            }
            if (int_or_default(ci->params, ctx->id("WRITE_WIDTH_A"), 0) == 1) {
                disconnect_port(ctx, ci, ctx->id("DIADI1"));
                connect_port(ctx, get_net_or_empty(ci, ctx->id("DIADI0")), ci, ctx->id("DIADI1"));
                ci->attrs[ctx->id("X_ORIG_PORT_DIADI1")] = std::string("DIADI[0]");
                disconnect_port(ctx, ci, ctx->id("DIPADIP0"));
                disconnect_port(ctx, ci, ctx->id("DIPADIP1"));
            }
            if (int_or_default(ci->params, ctx->id("WRITE_WIDTH_B"), 0) == 1) {
                disconnect_port(ctx, ci, ctx->id("DIBDI1"));
                connect_port(ctx, get_net_or_empty(ci, ctx->id("DIBDI0")), ci, ctx->id("DIBDI1"));
                ci->attrs[ctx->id("X_ORIG_PORT_DIBDI1")] = std::string("DIBDI[0]");
                disconnect_port(ctx, ci, ctx->id("DIPBDIP0"));
                disconnect_port(ctx, ci, ctx->id("DIPBDIP1"));
            }
            if (int_or_default(ci->params, ctx->id("WRITE_WIDTH_B"), 0) != 72) {
                for (std::string s : {"L", "U"}) {
                    for (int i = 4; i < 8; i++) {
                        IdString port = ctx->id("WEBWE" + s + std::to_string(i));
                        if (!ci->ports.count(port)) {
                            ci->ports[port].name = port;
                            ci->ports[port].type = PORT_IN;
                            connect_port(ctx, ctx->nets[ctx->id("$PACKER_GND_NET")].get(), ci, port);
                        }
                    }
                }
            } else {
                // Tie WEA low
                for (std::string s : {"L", "U"}) {
                    for (int i = 0; i < 4; i++) {
                        IdString port = ctx->id("WEA" + s + std::to_string(i));
                        if (!ci->ports.count(port)) {
                            ci->ports[port].name = port;
                            ci->ports[port].type = PORT_IN;
                            connect_port(ctx, ctx->nets[ctx->id("$PACKER_GND_NET")].get(), ci, port);
                        }
                    }
                }
            }
        }
    }
}

void USPacker::pack_uram()
{
    std::unordered_map<IdString, XFormRule> uram_rules;
    uram_rules[id_URAM288].new_type = id_BEL_URAM288;
    uram_rules[ctx->id("URAM288_BASE")] = uram_rules[id_URAM288];
    generic_xform(uram_rules, true);
}

void XilinxPacker::pack_inverters()
{
    // FIXME: fold where possible
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == ctx->id("INV")) {
            ci->params[ctx->id("INIT")] = Property(1, 2);
            rename_port(ctx, ci, ctx->id("I"), ctx->id("I0"));
            ci->type = ctx->id("LUT1");
        }
    }
}
void XC7Packer::pack_xadc()
{
    LogData logEntry8 = LogData::CreateLogStruct(
        LevelCode::INFO_LOG,
        LogCategory::PACK,
        PhaseType::PACK,
        "Packing xadc"
    );
    log_info("Packing xadc..\n",logEntry8);

    std::unordered_map<IdString, XFormRule> xadc_rules;
    xadc_rules[ctx->id("XADC")].new_type = id_XADC_XADC;
    // XADC映射
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if(ci->type == ctx->id("XADC")){
            fold_inverter(ci, "CONVSTCLK");
            fold_inverter(ci, "DCLK");
            xform_cell(xadc_rules,ci);
        }
    }
}

bool Arch::pack()
{
    if (xc7) {
        XC7Packer packer;
        packer.ctx = getCtx();
        packer.constrains_bel_loc();
        packer.pack_constants();
        packer.pack_inverters();
        packer.pack_io();
        // packer.prepare_iologic();
        packer.prepare_clocking();
        packer.pack_constants();
        packer.pack_iologic();
        packer.pack_idelayctrl();
        packer.pack_cfg();
        packer.pack_plls();
        packer.pack_gt();
        packer.pack_gbs();
        packer.pack_muxfs();
        packer.pack_carries();
        packer.pack_srls();
        packer.pack_dram();
        packer.pack_rom();
        packer.pack_bram();
        packer.pack_luts();
        packer.pack_dsps();
        packer.pack_xadc();
        packer.pack_ffs();
        packer.pack_cmt_fifo();
        packer.finalise_muxfs();
        packer.pack_lutffs();
        packer.link_clk_to_net();
        packer.constrains_bel_loc();
        packer.check();

    } else {
        USPacker packer;
        packer.ctx = getCtx();
        packer.pack_constants();
        packer.pack_inverters();
        packer.pack_io();
        packer.prepare_iologic();
        packer.prepare_clocking();
        packer.pack_constants();
        packer.pack_iologic();
        packer.pack_idelayctrl();
        packer.pack_clocking();
        packer.pack_muxfs();
        packer.pack_carries();
        packer.pack_luts();
        packer.pack_dram();
        packer.pack_bram();
        packer.pack_uram();
        packer.pack_dsps();
        packer.pack_ffs();
        packer.finalise_muxfs();
        packer.pack_lutffs();
    }

    assignArchInfo();
    attrs[id("step")] = std::string("pack");
    archInfoToAttributes();
    return true;
}

void Arch::assignCellInfo(CellInfo *cell)
{
    if (cell->type == id_SLICE_LUTX) {
        cell->lutInfo.input_count = 0;
        for (IdString a : {id_A1, id_A2, id_A3, id_A4, id_A5, id_A6}) {
            NetInfo *pn = get_net_or_empty(cell, a);
            if (pn != nullptr)
                cell->lutInfo.input_sigs[cell->lutInfo.input_count++] = pn;
        }
        cell->lutInfo.output_count = 0;
        for (IdString o : {id_O6, id_O5}) {
            NetInfo *pn = get_net_or_empty(cell, o);
            if (pn != nullptr)
                cell->lutInfo.output_sigs[cell->lutInfo.output_count++] = pn;
        }
        for (int i = cell->lutInfo.output_count; i < 2; i++)
            cell->lutInfo.output_sigs[i] = nullptr;
        cell->lutInfo.di1_net = get_net_or_empty(cell, id_DI1);
        cell->lutInfo.di2_net = get_net_or_empty(cell, id_DI2);
        cell->lutInfo.wclk = get_net_or_empty(cell, id_CLK);
        cell->lutInfo.memory_group = 0; // fixme
        cell->lutInfo.is_srl = cell->attrs.count(id("X_LUT_AS_SRL"));
        cell->lutInfo.is_memory = cell->attrs.count(id("X_LUT_AS_DRAM"));
        cell->lutInfo.only_drives_carry = false;
        if (xc7) {
            if (cell->constr_parent != nullptr && cell->lutInfo.output_count > 0 &&
                cell->lutInfo.output_sigs[0] != nullptr && cell->lutInfo.output_sigs[0]->users.size() == 1 &&
                cell->lutInfo.output_sigs[0]->users.at(0).cell->type == id_CARRY4)
                cell->lutInfo.only_drives_carry = true;
        } else {
            if (cell->constr_parent != nullptr && cell->lutInfo.output_count > 0 &&
                cell->lutInfo.output_sigs[0] != nullptr && cell->lutInfo.output_sigs[0]->users.size() == 1 &&
                cell->lutInfo.output_sigs[0]->users.at(0).cell->type == id_CARRY8)
                cell->lutInfo.only_drives_carry = true;
        }

        const IdString addr_msb_sigs[] = {id_WA7, id_WA8, id_WA9};
        for (int i = 0; i < 3; i++)
            cell->lutInfo.address_msb[i] = get_net_or_empty(cell, addr_msb_sigs[i]);

    } else if (cell->type == id_SLICE_FFX) {
        cell->ffInfo.d = get_net_or_empty(cell, id_D);
        cell->ffInfo.clk = get_net_or_empty(cell, xc7 ? id_CK : id_CLK);
        cell->ffInfo.ce = get_net_or_empty(cell, id_CE);
        cell->ffInfo.sr = get_net_or_empty(cell, id_SR);
        cell->ffInfo.is_clkinv = bool_or_default(cell->params, id("IS_CLK_INVERTED"), false);
        cell->ffInfo.is_srinv = bool_or_default(cell->params, id("IS_R_INVERTED"), false) ||
                                bool_or_default(cell->params, id("IS_S_INVERTED"), false) ||
                                bool_or_default(cell->params, id("IS_CLR_INVERTED"), false) ||
                                bool_or_default(cell->params, id("IS_PRE_INVERTED"), false);
        cell->ffInfo.is_latch = cell->attrs.count(id("X_FF_AS_LATCH"));
        cell->ffInfo.ffsync = cell->attrs.count(id("X_FFSYNC"));
    } else if (cell->type == id_F7MUX || cell->type == id_F8MUX || cell->type == id_F9MUX ||
               cell->type == id("SELMUX2_1")) {
        cell->muxInfo.sel = get_net_or_empty(cell, id_S0);
        cell->muxInfo.out = get_net_or_empty(cell, id_OUT);
    } else if (cell->type == id_CARRY8) {
        for (int i = 0; i < 8; i++) {
            cell->carryInfo.out_sigs[i] = get_net_or_empty(cell, id("O" + std::to_string(i)));
            cell->carryInfo.cout_sigs[i] = get_net_or_empty(cell, id("CO" + std::to_string(i)));
            cell->carryInfo.x_sigs[i] = get_net_or_empty(cell, id(std::string(1, 'A' + i) + "X"));
        }
    } else if (cell->type == id_CARRY4) {
        for (int i = 0; i < 4; i++) {
            cell->carryInfo.out_sigs[i] = get_net_or_empty(cell, id("O" + std::to_string(i)));
            cell->carryInfo.cout_sigs[i] = get_net_or_empty(cell, id("CO" + std::to_string(i)));
            cell->carryInfo.x_sigs[i] = nullptr;
        }
        cell->carryInfo.x_sigs[0] = get_net_or_empty(cell, id("CYINIT"));
    }
}

void Arch::assignArchInfo()
{
    for (auto cell : sorted(cells)) {
        assignCellInfo(cell.second);
    }
}

void XC7Packer::pack_cmt_fifo()
{
    LogData logEntry10 = LogData::CreateLogStruct(
        LevelCode::INFO_LOG,
        LogCategory::PACK,
        PhaseType::PACK,
        "Packing cmt fifo"
    );
    log_info("Packing cmt fifo..\n",logEntry10);

    std::unordered_map<IdString, XFormRule> cmt_fifo_rules;
    cmt_fifo_rules[ctx->id("IN_FIFO")].new_type = id_IN_FIFO_IN_FIFO;
    cmt_fifo_rules[ctx->id("OUT_FIFO")].new_type = id_OUT_FIFO_OUT_FIFO;

    for(int i = 0; i < 8; i++){
        cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("Q0[" + std::to_string(i) + "]"))] = ctx->id("Q0" + std::to_string(i));
        cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("Q1[" + std::to_string(i) + "]"))] = ctx->id("Q1" + std::to_string(i));
        cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("Q2[" + std::to_string(i) + "]"))] = ctx->id("Q2" + std::to_string(i));
        cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("Q3[" + std::to_string(i) + "]"))] = ctx->id("Q3" + std::to_string(i));
        cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("Q4[" + std::to_string(i) + "]"))] = ctx->id("Q4" + std::to_string(i));
        cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("Q5[" + std::to_string(i) + "]"))] = ctx->id("Q5" + std::to_string(i));
        cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("Q6[" + std::to_string(i) + "]"))] = ctx->id("Q6" + std::to_string(i));
        cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("Q7[" + std::to_string(i) + "]"))] = ctx->id("Q7" + std::to_string(i));
        cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("Q8[" + std::to_string(i) + "]"))] = ctx->id("Q8" + std::to_string(i));
        cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("Q9[" + std::to_string(i) + "]"))] = ctx->id("Q9" + std::to_string(i));

        cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("D0[" + std::to_string(i) + "]"))] = ctx->id("D0" + std::to_string(i));
        cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("D1[" + std::to_string(i) + "]"))] = ctx->id("D1" + std::to_string(i));
        cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("D2[" + std::to_string(i) + "]"))] = ctx->id("D2" + std::to_string(i));
        cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("D3[" + std::to_string(i) + "]"))] = ctx->id("D3" + std::to_string(i));
        cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("D4[" + std::to_string(i) + "]"))] = ctx->id("D4" + std::to_string(i));
        cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("D5[" + std::to_string(i) + "]"))] = ctx->id("D5" + std::to_string(i));
        cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("D6[" + std::to_string(i) + "]"))] = ctx->id("D6" + std::to_string(i));
        cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("D7[" + std::to_string(i) + "]"))] = ctx->id("D7" + std::to_string(i));
        cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("D8[" + std::to_string(i) + "]"))] = ctx->id("D8" + std::to_string(i));
        cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("D9[" + std::to_string(i) + "]"))] = ctx->id("D9" + std::to_string(i));

        if(i < 4){
            cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("D0[" + std::to_string(i) + "]"))] = ctx->id("D0" + std::to_string(i));
            cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("D1[" + std::to_string(i) + "]"))] = ctx->id("D1" + std::to_string(i));
            cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("D2[" + std::to_string(i) + "]"))] = ctx->id("D2" + std::to_string(i));
            cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("D3[" + std::to_string(i) + "]"))] = ctx->id("D3" + std::to_string(i));
            cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("D4[" + std::to_string(i) + "]"))] = ctx->id("D4" + std::to_string(i));
            cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("D5[" + std::to_string(i) + "]"))] = ctx->id("D5" + std::to_string(i));
            cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("D6[" + std::to_string(i) + "]"))] = ctx->id("D6" + std::to_string(i));
            cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("D7[" + std::to_string(i) + "]"))] = ctx->id("D7" + std::to_string(i));
            cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("D8[" + std::to_string(i) + "]"))] = ctx->id("D8" + std::to_string(i));
            cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("D9[" + std::to_string(i) + "]"))] = ctx->id("D9" + std::to_string(i));

            cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("Q0[" + std::to_string(i) + "]"))] = ctx->id("Q0" + std::to_string(i));
            cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("Q1[" + std::to_string(i) + "]"))] = ctx->id("Q1" + std::to_string(i));
            cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("Q2[" + std::to_string(i) + "]"))] = ctx->id("Q2" + std::to_string(i));
            cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("Q3[" + std::to_string(i) + "]"))] = ctx->id("Q3" + std::to_string(i));
            cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("Q4[" + std::to_string(i) + "]"))] = ctx->id("Q4" + std::to_string(i));
            cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("Q5[" + std::to_string(i) + "]"))] = ctx->id("Q5" + std::to_string(i));
            cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("Q6[" + std::to_string(i) + "]"))] = ctx->id("Q6" + std::to_string(i));
            cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("Q7[" + std::to_string(i) + "]"))] = ctx->id("Q7" + std::to_string(i));
            cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("Q8[" + std::to_string(i) + "]"))] = ctx->id("Q8" + std::to_string(i));
            cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("Q9[" + std::to_string(i) + "]"))] = ctx->id("Q9" + std::to_string(i));
        }

    }
    cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("RDCLK"))] = ctx->id("RDCLK");
    cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("RDEN"))]  = ctx->id("RDEN");
    cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("RESET"))] = ctx->id("RESET");
    cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("WRCLK"))] = ctx->id("WRCLK");
    cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("WREN"))]  = ctx->id("WREN");
    cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("ALMOSTEMPTY"))]  = ctx->id("ALMOSTEMPTY");
    cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("ALMOSTFULL"))]  = ctx->id("ALMOSTFULL");
    cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("EMPTY"))]  = ctx->id("EMPTY");
    cmt_fifo_rules[ctx->id("IN_FIFO")].port_xform[ctx->id(std::string("FULL"))]  = ctx->id("FULL");

    cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("RDCLK"))] = ctx->id("RDCLK");
    cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("RDEN"))]  = ctx->id("RDEN");
    cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("RESET"))] = ctx->id("RESET");
    cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("WRCLK"))] = ctx->id("WRCLK");
    cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("WREN"))]  = ctx->id("WREN");
    cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("ALMOSTEMPTY"))]  = ctx->id("ALMOSTEMPTY");
    cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("ALMOSTFULL"))]  = ctx->id("ALMOSTFULL");
    cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("EMPTY"))]  = ctx->id("EMPTY");
    cmt_fifo_rules[ctx->id("OUT_FIFO")].port_xform[ctx->id(std::string("FULL"))]  = ctx->id("FULL");
    
    generic_xform(cmt_fifo_rules, true);
}

// Add capturing clk to nets
void XC7Packer::link_clk_to_net() {
    /***************************************************************************************
    / Note:
    / No internal clk source on existing devices, so no need to iterate through PLL or MMCM
    /***************************************************************************************/

    // 统计所有时钟器件
    std::set<IdString> clk_cells = {id_IOB33M_INBUF_EN, id_IOB33_INBUF_EN, id_BUFGCTRL, id_BUFHCE_BUFHCE, id_BUFIO_BUFIO, id_BUFMRCE_BUFMRCE, id_BUFR_BUFR, id_PLLE2_ADV_PLLE2_ADV, id_MMCME2_ADV_MMCME2_ADV};

    // Iterate clk tree, pass clk properties
    for (NetInfo *src_clk : ctx->global_clks) {
        std::vector<NetInfo *> clk_nets;
        clk_nets.push_back(src_clk);

        while (!clk_nets.empty()) {
            auto clk_net = clk_nets.back();
            clk_nets.pop_back();
            // 遍历net的users
            for (PortRef port_ref : clk_net->users) {
                CellInfo *ci = port_ref.cell;
                // 如果cell属于clk_cells
                if (clk_cells.find(ci->type) == clk_cells.end())
                    continue;
                // 根据不同类型，取出对应的输出端口
                std::vector<NetInfo *> output_clks;
                if (ci->type == id_IOB33M_INBUF_EN || ci->type == id_IOB33_INBUF_EN)
                    output_clks.push_back(ci->ports[ctx->id("OUT")].net);
                else if (ci->type == id_BUFGCTRL || ci->type == id_BUFHCE_BUFHCE || ci->type == id_BUFIO_BUFIO || ci->type == id_BUFMRCE_BUFMRCE || ci->type == id_BUFR_BUFR) {
                    output_clks.push_back(ci->ports[ctx->id("O")].net);
                } else if (ci->type == id_PLLE2_ADV_PLLE2_ADV || ci->type == id_MMCME2_ADV_MMCME2_ADV) {
                    for (int i = 0; i < 7; i++) {
                        std::string pn = "CLKOUT" + std::to_string(i);
                        if (ci->ports.find(ctx->id(pn)) != ci->ports.end())
                            output_clks.push_back(ci->ports[ctx->id(pn)].net);
                        if (ci->ports.find(ctx->id(pn+"B")) != ci->ports.end())
                            output_clks.push_back(ci->ports[ctx->id(pn+"B")].net);
                    }
                }
                // 根据cell和输出端口，填入clk信息
                for (NetInfo *out_clk: output_clks) {
                    out_clk->is_clk = true;
                    out_clk->is_direved_clk = clk_net->is_direved_clk;
                    out_clk->clkconstr = std::unique_ptr<ClockConstraint>(new ClockConstraint);
                    out_clk->clkconstr->high = clk_net->clkconstr->high;
                    out_clk->clkconstr->low = clk_net->clkconstr->low;
                    out_clk->clkconstr->period = clk_net->clkconstr->period; // 默认直接传递
                    if (ci->type == id_BUFR_BUFR) {
                        std::string d = str_or_default(ci->params, ctx->id("BUFR_DIVIDE"), "BYPASS");
                        if (d != "BYPASS") {
                            int devider = std::stoi(d);
                            out_clk->clkconstr->period.min_delay = src_clk->clkconstr->period.minDelay() * devider;
                            out_clk->clkconstr->period.max_delay = src_clk->clkconstr->period.maxDelay() * devider;
                            out_clk->is_direved_clk = true;
                        }
                    } else if (ci->type == id_PLLE2_ADV_PLLE2_ADV ) {
                        int M = int_or_default(ci->params, ctx->id("CLKFBOUT_MULT"), 1);
                        int D = int_or_default(ci->params, ctx->id("DIVCLK_DIVIDE"), 1);
                        int O_n = int_or_default(ci->params, ctx->id(out_clk->driver.port.str(ctx)+"_DIVIDE"), 1);
                        out_clk->clkconstr->period.min_delay = (clk_net->clkconstr->period.minDelay() / M) * D * O_n * 1.0;
                        out_clk->clkconstr->period.max_delay = (clk_net->clkconstr->period.maxDelay() / M) * D * O_n * 1.0;
                        out_clk->is_direved_clk = true;
                    } else if (ci->type == id_MMCME2_ADV_MMCME2_ADV) {
                        float M = std::stof(str_or_default(ci->params, ctx->id("CLKFBOUT_MULT_F"), "1"));
                        int D = int_or_default(ci->params, ctx->id("DIVCLK_DIVIDE"), 1);
                        std::string port_name = out_clk->driver.port.str(ctx);
                        if (port_name.back() == 'B')
                            port_name.pop_back();
                        port_name+="_DIVIDE";
                        float O_n;
                        if (port_name == "CLKOUT0_DIVIDE")
                            O_n = std::stof(str_or_default(ci->params, ctx->id(port_name+"_F"), "1"));
                        else 
                            O_n = float(int_or_default(ci->params, ctx->id(port_name), 1));
                        out_clk->clkconstr->period.min_delay = (clk_net->clkconstr->period.minDelay() / M) * D * O_n * 1.0;
                        out_clk->clkconstr->period.max_delay = (clk_net->clkconstr->period.maxDelay() / M) * D * O_n * 1.0;
                        out_clk->is_direved_clk = true;
                    }
                }
                clk_nets.insert(clk_nets.end(), std::make_move_iterator(output_clks.begin()),std::make_move_iterator(output_clks.end()));
            }
        }
    }

    // Iterate all cells, and set input nets capturing_clk
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;

        NetInfo *clk_in = nullptr;
        for (auto port: ci->ports) {
            if (port.second.net && port.second.type == PORT_IN && port.second.net->is_clk ){
                clk_in = port.second.net;
                break;
            }
        }
        // If no clk input, use global clk
        if (!clk_in) 
            clk_in = GetFastGlobelClk(ctx);

        // If found clk, set capturing clk on each input port
        if (clk_in) 
            for (auto port: ci->ports) 
                if (port.second.net && port.second.type == PORT_IN && !port.second.net->is_clk)
                    port.second.net->capturing_clk = clk_in;
    }
}

NEXTPNR_NAMESPACE_END
