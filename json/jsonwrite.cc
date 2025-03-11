/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Miodrag Milanovic <miodrag@symbioticeda.com>
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

#include "jsonwrite.h"
#include <assert.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <log.h>
#include <map>
#include <string>
#include "nextpnr.h"
#include "version.h"
#ifdef COMPRESS_MODE
#include "ArchiveTool.h"
#endif

#include "json.hpp"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace JsonWriter {

std::string get_string(std::string str)
{
    std::string newstr = "\"";
    for (char c : str) {
        if (c == '\\')
            newstr += c;
        newstr += c;
    }
    return newstr + "\"";
}

std::string get_name(IdString name, Context *ctx) { return get_string(name.c_str(ctx)); }

void write_parameters(std::ostream &f, Context *ctx, const std::unordered_map<IdString, Property> &parameters,
                      bool for_module = false)
{
    bool first = true;
    for (auto &param : parameters) {
        f << stringf("%s\n", first ? "" : ",");
        f << stringf("        %s%s: ", for_module ? "" : "    ", get_name(param.first, ctx).c_str());
        f << get_string(param.second.to_string());
        first = false;
    }
}

struct PortGroup
{
    std::string name;
    std::vector<int> bits;
    PortType dir;
};

std::vector<PortGroup> group_ports(Context *ctx, const std::unordered_map<IdString, PortInfo> &ports,
                                   bool is_cell = false)
{
    std::vector<PortGroup> groups;
    std::unordered_map<std::string, size_t> base_to_group;
    for (auto &pair : ports) {
        std::string name = pair.second.name.str(ctx);
        if ((name.back() != ']') || (name.find('[') == std::string::npos)) {
            groups.push_back({name,
                              {is_cell ? (pair.second.net ? pair.second.net->name.index : -1) : pair.first.index},
                              pair.second.type});
        } else {
            int off1 = int(name.find_last_of('['));
            std::string basename = name.substr(0, off1);
            int index = std::stoi(name.substr(off1 + 1, name.size() - (off1 + 2)));

            if (!base_to_group.count(basename)) {
                base_to_group[basename] = groups.size();
                groups.push_back({basename, std::vector<int>(index + 1, -1), pair.second.type});
            }

            auto &grp = groups.at(base_to_group[basename]);
            if (int(grp.bits.size()) <= index)
                grp.bits.resize(index + 1, -1);
            NPNR_ASSERT(grp.bits.at(index) == -1);
            grp.bits.at(index) = pair.second.net ? pair.second.net->name.index : (is_cell ? -1 : pair.first.index);
        }
    }
    return groups;
}

std::string format_port_bits(const PortGroup &port, int &dummy_idx)
{
    std::stringstream s;
    s << "[ ";
    bool first = true;
    if (port.bits.size() != 1 || port.bits.at(0) != -1) // skip single disconnected ports
        for (auto bit : port.bits) {
            if (!first)
                s << ", ";
            if (bit == -1)
                s << (++dummy_idx);
            else
                s << bit;
            first = false;
        }
    s << " ]";
    return s.str();
}

void write_module(std::ostream &f, Context *ctx)
{
    auto val = ctx->attrs.find(ctx->id("module"));
    int dummy_idx = int(ctx->idstring_idx_to_str->size()) + 1000;
    if (val != ctx->attrs.end())
        f << stringf("    %s: {\n", get_string(val->second.as_string()).c_str());
    else
        f << stringf("    %s: {\n", get_string("top").c_str());
    f << stringf("      \"settings\": {");
    write_parameters(f, ctx, ctx->settings, true);
    f << stringf("\n      },\n");
    f << stringf("      \"attributes\": {");
    write_parameters(f, ctx, ctx->attrs, true);
    f << stringf("\n      },\n");
    f << stringf("      \"ports\": {");

    auto ports = group_ports(ctx, ctx->ports);
    bool first = true;
    for (auto &port : ports) {
        f << stringf("%s\n", first ? "" : ",");
        f << stringf("        %s: {\n", get_string(port.name).c_str());
        f << stringf("          \"direction\": \"%s\",\n", port.dir == PORT_IN      ? "input"
                                                           : port.dir == PORT_INOUT ? "inout"
                                                                                    : "output");
        f << stringf("          \"bits\": %s\n", format_port_bits(port, dummy_idx).c_str());
        f << stringf("        }");
        first = false;
    }
    f << stringf("\n      },\n");

    f << stringf("      \"cells\": {");
    first = true;
    for (auto &pair : ctx->cells) {
        auto &c = pair.second;
        auto cell_ports = group_ports(ctx, c->ports, true);
        f << stringf("%s\n", first ? "" : ",");
        f << stringf("        %s: {\n", get_name(c->name, ctx).c_str());
        f << stringf("          \"hide_name\": %s,\n", c->name.c_str(ctx)[0] == '$' ? "1" : "0");
        f << stringf("          \"type\": %s,\n", get_name(c->type, ctx).c_str());
        f << stringf("          \"parameters\": {");
        write_parameters(f, ctx, c->params);
        f << stringf("\n          },\n");
        f << stringf("          \"attributes\": {");
        write_parameters(f, ctx, c->attrs);
        f << stringf("\n          },\n");
        f << stringf("          \"port_directions\": {");
        bool first2 = true;
        for (auto &pg : cell_ports) {
            std::string direction = (pg.dir == PORT_IN) ? "input" : (pg.dir == PORT_OUT) ? "output" : "inout";
            f << stringf("%s\n", first2 ? "" : ",");
            f << stringf("            %s: \"%s\"", get_string(pg.name).c_str(), direction.c_str());
            first2 = false;
        }
        f << stringf("\n          },\n");
        f << stringf("          \"connections\": {");
        first2 = true;
        for (auto &pg : cell_ports) {
            f << stringf("%s\n", first2 ? "" : ",");
            f << stringf("            %s: %s", get_string(pg.name).c_str(), format_port_bits(pg, dummy_idx).c_str());
            first2 = false;
        }
        f << stringf("\n          }\n");

        f << stringf("        }");
        first = false;
    }

    f << stringf("\n      },\n");

    f << stringf("      \"netnames\": {");
    first = true;
    for (auto &pair : ctx->nets) {
        auto &w = pair.second;
        f << stringf("%s\n", first ? "" : ",");
        f << stringf("        %s: {\n", get_name(w->name, ctx).c_str());
        f << stringf("          \"hide_name\": %s,\n", w->name.c_str(ctx)[0] == '$' ? "1" : "0");
        f << stringf("          \"bits\": [ %d ] ,\n", pair.first.index);
        f << stringf("          \"attributes\": {");
        write_parameters(f, ctx, w->attrs);
        f << stringf("\n          }\n");
        f << stringf("        }");
        first = false;
    }

    f << stringf("\n      }\n");
    f << stringf("    }");
}

void write_context_back(std::ostream &f, Context *ctx)
{
    f << stringf("{\n");
    f << stringf("  \"creator\": %s,\n",
                 get_string("Next Generation Place and Route (Version " GIT_DESCRIBE_STR ")").c_str());
    f << stringf("  \"modules\": {\n");
    write_module(f, ctx);
    f << stringf("\n  }");
    f << stringf("\n}\n");
}

void write_context(std::ostream &f, Context *ctx)
{
    nlohmann::ordered_json ctx_json;
    ctx_json["creator"] = "Next Generation Place and Route (Version " GIT_DESCRIBE_STR ")";
    int dummy_idx = int(ctx->idstring_idx_to_str->size()) + 1000;
    // 
    auto top_name = str_or_default(ctx->attrs, ctx->id("module"), "top");
    nlohmann::ordered_json top_module_json;
    // 遍历settings
    nlohmann::ordered_json setting_json = nlohmann::ordered_json::object();
    for (auto &setting : ctx->settings) {
        setting_json[setting.first.c_str(ctx)] = setting.second.to_string();
    }
    top_module_json["settings"] = setting_json;
    // 遍历attributes
    nlohmann::ordered_json attributes_json = nlohmann::ordered_json::object();
    for (auto &attr : ctx->attrs) {
        attributes_json[attr.first.c_str(ctx)] = attr.second.to_string();
    }
    top_module_json["attributes"] = attributes_json;
    // 遍历 top_ports
    nlohmann::ordered_json ports_json = nlohmann::ordered_json::object();
    auto ports = group_ports(ctx, ctx->ports);
    for (auto &port : ports) {
        nlohmann::json bits = nlohmann::json::array();
        if (port.bits.size() != 1 || port.bits.at(0) != -1) // skip single disconnected ports
            for (auto bit : port.bits) {
                if (bit == -1)
                    bits.push_back(++dummy_idx);
                else
                    bits.push_back(bit);
            }
        ports_json[port.name] = nlohmann::ordered_json{
                {"direction", port.dir == PORT_IN ? "input" : port.dir == PORT_INOUT ? "inout" : "output"},
                {"bits", bits}
            };
    }
    top_module_json["ports"] = ports_json;
    // 遍历cells
    nlohmann::ordered_json cells_json = nlohmann::ordered_json::object();
    for (auto &pair : ctx->cells) {
        nlohmann::ordered_json cell_json = nlohmann::ordered_json::object();;
        auto &c = pair.second;
        auto cell_ports = group_ports(ctx, c->ports, true);
        cell_json["hide_name"] = c->name.c_str(ctx)[0] == '$' ? 1 : 0;
        cell_json["type"] = ctx->nameOf(c->type);
        nlohmann::ordered_json parameters_json = nlohmann::ordered_json::object();
        for (auto &param : c->params) {
            parameters_json[param.first.c_str(ctx)] = param.second.to_string();
        }
        cell_json["parameters"] = parameters_json;
        nlohmann::ordered_json attributes_json = nlohmann::ordered_json::object();
        for (auto &attr : c->attrs) {
            attributes_json[attr.first.c_str(ctx)] = attr.second.to_string();
        }
        cell_json["attributes"] = attributes_json;
        nlohmann::ordered_json port_directions_json = nlohmann::ordered_json::object();
        for (auto &pg : cell_ports) {
            std::string direction = (pg.dir == PORT_IN) ? "input" : (pg.dir == PORT_OUT) ? "output" : "inout";
            port_directions_json[pg.name] = direction;
        }
        cell_json["port_directions"] = port_directions_json;
        nlohmann::ordered_json connections_json = nlohmann::ordered_json::object();
        for (auto &pg : cell_ports) {
            nlohmann::json bits = nlohmann::json::array();
            if (pg.bits.size() != 1 || pg.bits.at(0) != -1) // skip single disconnected ports
                for (auto bit : pg.bits) {
                    if (bit == -1)
                        bits.push_back(++dummy_idx);
                    else
                        bits.push_back(bit);
                }
            connections_json[pg.name] = bits;
        }
        log_info(connections_json.dump().c_str());
        cell_json["connections"] = connections_json;
        cells_json[ctx->nameOf(c->name)] = cell_json;
    }
    top_module_json["cells"] = cells_json;
    // 遍历netnames
    nlohmann::ordered_json netnames_json = nlohmann::ordered_json::object();
    for (auto &pair : ctx->nets) {
        auto &w = pair.second;
        nlohmann::ordered_json net_json;
        net_json["hide_name"] = w->name.c_str(ctx)[0] == '$' ? 1 : 0;
        net_json["bits"] = nlohmann::json::array({pair.first.index});
        nlohmann::ordered_json attributes_json = nlohmann::ordered_json::object();
        for (auto &attr : w->attrs) {
            attributes_json[attr.first.c_str(ctx)] = attr.second.to_string();
        }
        net_json["attributes"] = attributes_json;
        netnames_json[ctx->nameOf(w->name)] = net_json;
    }
    top_module_json["netnames"] = netnames_json;

    ctx_json["modules"] = nlohmann::ordered_json{
        {"top",top_module_json}
    };
    f<<ctx_json.dump(4);
}

}; // End Namespace JsonWriter

bool write_json_file(std::ostream &f, std::string &filename, Context *ctx)
{
    try {
        using namespace JsonWriter;
        if (!f)
            log_error("failed to open JSON file.\n");
        if(ctx->compress_mode){
#ifdef COMPRESS_MODE
            std::ostringstream os_buffer;
            write_context(os_buffer, ctx);
            std::istringstream inStream(os_buffer.str());
            Tool::ArchiveTool tool;
            std::string use_filename;
            size_t lastSlash = filename.find_last_of("/\\");
            if(lastSlash == std::string::npos){
                use_filename = filename;
            }else{
                // 获取文件名部分
                use_filename = filename.substr(lastSlash + 1);
            }
            tool.compressWithPassword(inStream,f,use_filename,KEY);
#endif
        }else{
            write_context(f, ctx);
        }
        log_break();
        return true;
    } catch (log_execution_error_exception) {
        return false;
    }
}

NEXTPNR_NAMESPACE_END
