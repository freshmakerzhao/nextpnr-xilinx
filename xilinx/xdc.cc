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

#include "log.h"
#include "nextpnr.h"
#include <fstream>
#include "json.hpp"
#include "util.h"
NEXTPNR_NAMESPACE_BEGIN

// 判断字符串是不是一个合法的8位16进制数
bool is_valid_hex32(const std::string &str) {
    std::string hex_digits = "0123456789abcdefABCDEF";
    // Check for optional "0x" or "0X" prefix
    assert(str.size() == 10);
    assert((str[0] == '0' && (str[1] == 'x' || str[1] == 'X')));
    // Check if all characters are valid hex digits
    for (size_t i = 2; i < str.size(); ++i) {
        if (hex_digits.find(str[i]) == std::string::npos) {
            return false;
        }
    }
    return true;
}

void Arch::parseXdc(std::istream &in)
{

    if (!in)
        log_error("failed to open LPF file\n");
    std::string line;
    std::string linebuf;
    int lineno = 0;

    auto isempty = [](const std::string &str) {
        return std::all_of(str.begin(), str.end(), [](char c) { return std::isspace(c); });
    };
    auto strip_quotes = [](const std::string &str) {
        if (str.at(0) == '"') {
            NPNR_ASSERT(str.back() == '"');
            return str.substr(1, str.size() - 2);
        } else if (str.at(0) == '{') {
            NPNR_ASSERT(str.back() == '}');
            return str.substr(1, str.size() - 2);
        } else {
            return str;
        }
    };
    auto split_to_args = [](const std::string &str, bool group_brackets) {
        std::vector<std::string> split_args;
        std::string buffer;
        auto flush = [&]() {
            if (!buffer.empty())
                split_args.push_back(buffer);
            buffer.clear();
        };
        int brcount = 0;
        for (char c : str) {
            if ((c == '[' || c == '{') && group_brackets) {
                ++brcount;
            }
            if ((c == ']' || c == '}') && group_brackets) {
                --brcount;
                buffer += c;
                if (brcount == 0)
                    flush();
                continue;
            }
            if (std::isspace(c)) {
                if (brcount == 0) {
                    flush();
                    continue;
                }
            }
            buffer += c;
        }
        flush();
        return split_args;
    };
    auto get_cells_name = [&](std::string str) {
        std::vector<CellInfo *> tgt_cells;
        if (str.empty() || str.front() != '[')
            log_error("failed to parse target (on line %d)\n", lineno);
        str = str.substr(1, str.size() - 2);
        auto split = split_to_args(str, false);
        if (split.size() < 1)
            log_error("failed to parse target (on line %d)\n", lineno);
        if (split.front() != "get_ports" && split.front() != "get_cells")
            log_error("targets other than 'get_ports' are not supported (on line %d)\n", lineno);
        if (split.size() < 2)
            log_error("failed to parse target (on line %d)\n", lineno);
        std::string cellname = strip_quotes(split.at(1));
        return cellname;
    };

    auto get_cells = [&](std::string str) {
        std::vector<CellInfo *> tgt_cells;
        if (str.empty() || str.front() != '[')
            log_error("failed to parse target (on line %d)\n", lineno);
        str = str.substr(1, str.size() - 2);
        auto split = split_to_args(str, false);
        if (split.size() < 1)
            log_error("failed to parse target (on line %d)\n", lineno);
        if (split.front() != "get_ports" && split.front() != "get_cells")
            log_error("targets other than 'get_ports' are not supported (on line %d)\n", lineno);
        if (split.size() < 2)
            log_error("failed to parse target (on line %d)\n", lineno);
        IdString cellname = id(strip_quotes(split.at(1)));
        if (cells.count(cellname))
            tgt_cells.push_back(cells.at(cellname).get());      
        return tgt_cells;
    };

    auto get_nets = [&](std::string str) {
        std::vector<NetInfo *> tgt_nets;
        if (str.empty() || str.front() != '[')
            log_error("failed to parse target (on line %d)\n", lineno);
        str = str.substr(1, str.size() - 2);
        auto split = split_to_args(str, false);
        if (split.size() < 1)
            log_error("failed to parse target (on line %d)\n", lineno);
        if (split.front() != "get_ports" && split.front() != "get_nets")
            log_error("targets other than 'get_ports' or 'get_nets' are not supported (on line %d)\n", lineno);
        if (split.size() < 2)
            log_error("failed to parse target (on line %d)\n", lineno);
        IdString netname = id(split.at(1));
        NetInfo *maybe_net = getNetByAlias(netname);
        if (maybe_net != nullptr)
            tgt_nets.push_back(maybe_net);
        return tgt_nets;
    };

    Context *ctx = getCtx();
    while (std::getline(in, line)) {
        ++lineno;
        // Trim comments, from # until end of the line
        size_t cstart = line.find('#');
        if (cstart != std::string::npos)
            line = line.substr(0, cstart);
        if (isempty(line))
            continue;
           
        std::vector<std::string> arguments = split_to_args(line, true);
        if (arguments.empty())
            continue;
        std::string &cmd = arguments.front();
        if (cmd == "set_property") {
            std::vector<std::pair<std::string, std::string>> arg_pairs;
            if (arguments.size() != 4)
                log_error("expected four arguments to 'set_property' (on line %d)\n", lineno);
            else if (arguments.at(1) == "-dict") {
                std::vector<std::string> dict_args = split_to_args(strip_quotes(arguments.at(2)), false);
                if ((dict_args.size() % 2) != 0)
                    log_error("expected an even number of argument for dictionary (on line %d)\n", lineno);
                arg_pairs.reserve(dict_args.size() / 2);
                for (int cursor = 0; cursor + 1 < int(dict_args.size()); cursor += 2) {
                    arg_pairs.emplace_back(std::move(dict_args.at(cursor)), std::move(dict_args.at(cursor + 1)));
                }
            } else
                arg_pairs.emplace_back(std::move(arguments.at(1)), std::move(arguments.at(2)));
            if (arguments.at(1) == "INTERNAL_VREF")
                continue;
            if (arguments.at(3).size() > 2 && arguments.at(3) == "[current_design]") {
                // 打开文件并读取现有内容
                std::ifstream infile("order.json");
                nlohmann::json json_content;
                if (infile.is_open()) {
                    try {
                        infile >> json_content;  // 读取并解析现有的 JSON 文件
                    } catch (...) {
                        log_error("[Implementation_pack_hybrd]：Failed to parse the existing order.json file\n");
                    }
                    infile.close();
                }
                // 判断pair.first是否包含BITSTREAM.CONFIG.USR_ACCESS
                std::string first_key = std::get<0>(arg_pairs[0]);
                if(first_key=="BITSTREAM.CONFIG.USR_ACCESS") {
                    // 判断pair.second是不是一个32位的16进制数
                    std::string first_value = std::get<1>(arg_pairs[0]);
                    bool paramter_is_valid = is_valid_hex32(first_value);
                    if(paramter_is_valid){
                        json_content["usr_access_value"] = nlohmann::json(std::get<1>(arg_pairs[0]));   
                    }else{
                        log_error("[Implementation_pack_hybrd]:The 8-bit hexadecimal number format is incorrect\n");
                        return;
                    }
                } else if(first_key=="POST_CRC") {
                    std::string first_value = std::get<1>(arg_pairs[0]);
                    if(first_value == "ENABLE" || first_value == "DISABLE"){
                        ctx->settings[ctx->id("POST_CRC")] = first_value;
                        json_content["POST_CRC"]=nlohmann::json(std::get<1>(arg_pairs[0]));
                    }else {
                        log_error("[Implementation_pack_hybrd]: POST_CRC has the ENABLE or DISABLE attributes\n");
                    }
                } else if(first_key=="POST_CRC_SOURCE") {
                    std::string first_value = std::get<1>(arg_pairs[0]);
                    if(first_value == "PRE_COMPUTED" || first_key=="FIRST_READBACK"){
                        json_content["POST_CRC_SOURCE"]=nlohmann::json(std::get<1>(arg_pairs[0]));
                    }else{
                        log_error("[Implementation_pack_hybrd]: POST_CRC_SOURCE value must PRE_COMPUTED or FIRST_READBACK attributes\n");
                    }
                } else if(first_key=="POST_CRC_FREQ") {
                    std::string first_value = std::get<1>(arg_pairs[0]);
                    if(first_value == "{1}" || first_value == "{2}" || first_value == "{3}" || first_value == "{4}" || first_value == "{6}" || first_value == "{7}"
                    || first_value == "{8}" || first_value == "{10}" || first_value == "{12}" || first_value == "{13}" || first_value == "{16}" || first_value == "{17}"
                    || first_value == "{22}" || first_value == "{25}" || first_value == "{26}" || first_value == "{27}" || first_value == "{33}" || first_value == "{40}"
                    || first_value == "{44}" || first_value == "{50}" || first_value == "{66}" || first_value == "{100}"){
                        json_content["POST_CRC_FREQ"]=nlohmann::json(std::get<1>(arg_pairs[0]));
                    }else{
                        log_error("[Implementation_pack_hybrd]: POST_CRC_FREQ value must one of the 1 2 3 4 6 7 8 10 12 13 16 17 22 25 26 27 33 40 44 50 66 100\n");
                    }
                } else if(first_key=="CONFIG_MODE") {
                    std::string first_value = std::get<1>(arg_pairs[0]);
                    if(first_value == "SPIx1" || first_value == "SPIx2" || first_value == "SPIx4" || first_value == "BPI8" || first_value == "BPI16" || first_value == "S_SELECTMAP16+READBACK" ||
                       first_value == "S_SELECTMAP16" || first_value == "S_SELECTMAP32+READBACK" || first_value == "S_SELECTMAP32" || first_value == "B_SCAN+READBACK" || 
                       first_value == "M_SELECTMAP+READBACK" || first_value == "S_SELECTMAP+READBACK" || first_value == "B_SCAN" || first_value == "M_SELECTMAP" ||
                       first_value == "S_SELECTMAP" || first_value == "M_SERIAL" || first_value == "S_SERIAL"){
                        json_content["CONFIG_MODE"]=nlohmann::json(std::get<1>(arg_pairs[0]));
                    }else{
                        log_error("[Implementation_pack_hybrd]: CONFIG_MODE value must one of the SPIx1 SPIx2 SPIx4 BPI8 BPI16  S_SERIAL M_SERIAL S_SELECTMAP M_SELECTMAP B_SCAN S_SELECTMAP+READBACK M_SELECTMAP+READBACK B_SCAN+READBACK S_SELECTMAP32 S_SELECTMAP32+READBACK S_SELECTMAP16 S_SELECTMAP16+READBACK  attributes\n");
                    }
                }
                 // 将合并后的 JSON 对象写回到文件中
                std::ofstream outfile("order.json");
                if (!outfile.is_open()) {
                    log_error("[Implementation_pack_hybrd]: Not Create or Open the order.json file\n");
                } else {
                    outfile << json_content.dump(4);  // 以 4 个空格缩进的格式写入 JSON
                    outfile.close();
                }
                continue;
            } else if(std::get<0>(arg_pairs[0])=="BEL") {
                Property bel_type = std::get<1>(arg_pairs[0]);
                IdString cell_name = id(get_cells_name(arguments.at(3)));
                ctx->constrains[cell_name][id("BEL_TYPE")] = bel_type;

             } else if(std::get<0>(arg_pairs[0])=="LOC") {
                Property bel_loc = std::get<1>(arg_pairs[0]);
                IdString cell_name = id(get_cells_name(arguments.at(3)));
                ctx->constrains[cell_name][id("LOC")] = bel_loc;
            }
            std::vector<CellInfo *> dest = get_cells(arguments.at(3));
            for (auto c : dest)
                for (const auto &pair : arg_pairs)
                    c->attrs[id(pair.first)] = std::string(pair.second);
            for (auto cell : sorted(ctx->cells)) {
                CellInfo *ci = cell.second;
                if(ci->attrs.count(ctx->id("BEL")) && ci->attrs.count(ctx->id("LOC")))
                    ci->attrs[ctx->id("BEL")] = ci->attrs.at(ctx->id("LOC")).as_string() + "/" + ci->attrs.at(ctx->id("BEL")).as_string();
            }

        } else if (cmd == "create_clock") {
            double period = 0;
            bool got_period = false;
            int cursor = 1;
            for (cursor = 1; cursor < int(arguments.size()); cursor++) {
                std::string opt = arguments.at(cursor);
                if (opt == "-add")
                    ;
                else if (opt == "-name" || opt == "-waveform")
                    cursor++;
                else if (opt == "-period") {
                    cursor++;
                    period = std::stod(arguments.at(cursor));
                    got_period = true;
                } else
                    break;
            }
            if (!got_period)
                log_error("found create_clock without period (on line %d)", lineno);
            std::vector<NetInfo *> dest = get_nets(arguments.at(cursor));
            for (auto n : dest) {
                n->clkconstr = std::unique_ptr<ClockConstraint>(new ClockConstraint);
                n->clkconstr->period = getDelayFromNS(period);
                n->clkconstr->high.delay = n->clkconstr->period.delay / 2;
                n->clkconstr->low.delay = n->clkconstr->period.delay / 2;
            }
        } else {
#ifdef HYBRDLINK
            log_info("ignoring unsupported HDC command '%s' (on line %d)\n", cmd.c_str(), lineno);
#else
            log_info("ignoring unsupported XDC command '%s' (on line %d)\n", cmd.c_str(), lineno);    
#endif
        }
    }
    if (!isempty(linebuf)){
#ifdef HYBRDLINK
        log_error("unexpected end of HDC file\n");
#else
        log_error("unexpected end of XDC file\n");
#endif
    }
}

NEXTPNR_NAMESPACE_END
