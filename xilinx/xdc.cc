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
#include <regex>
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

std::string trim_and_reduce_spaces(std::string line) {
    // 将中间的多个空格替换为单个空格
    std::regex multiple_spaces("\\s+"); // 匹配一个或多个空白字符
    line = std::regex_replace(line, multiple_spaces, " ");
    // 去除首尾空格
    line.erase(0, line.find_first_not_of(" "));
    line.erase(line.find_last_not_of(" ") + 1);
    return line;
}

std::vector<std::string> split_to_args(std::string line, bool trim_sq=false) {
    std::vector<std::string> args;
    std::string buffer;
    int brcount = 0; //存中括号
    if (trim_sq) {
        line = trim_and_reduce_spaces(line);
        if (line[0] == '[' || line[0] == '"' || line[0] == '{') {
            line = line.substr(1, line.size() - 2);
            line = trim_and_reduce_spaces(line);
        }
            
    }
    for (char c : line) {
        if (c == ' ') {
            if (brcount == 0 && !buffer.empty()) {
                args.push_back(buffer);
                buffer.clear();
            } else {
                buffer += c;
            }
        } else if (c == '[' || c == '{') {
            brcount++;
            buffer += c;
        } else if (c == ']' || c == '}') {
            brcount--;
            buffer += c;
        } else {
            buffer +=c;
        }
    }
    if (!buffer.empty()){
        args.push_back(buffer);
    }
    return args;
}


std::vector<std::string> get_cellnames(std::string str) {
    std::vector<std::string> result;
    std::vector<std::string> args = split_to_args(str, true);
    if (args.size()>1) {
        log_assert(args[0] == "list");
        // 多个信号
        args.erase(args.begin());
    }
    // 解析出所有cell, 带[]的遍历出来
    std::regex pattern(R"(([a-zA-Z][a-zA-Z0-9_]*)\[(\d+):(\d+)\])");
    std::smatch match;
    for( std::string item: args) {
        if (std::regex_search(item, match, pattern)) {
            std::string var_name = match[1];   // 变量名
            int high = std::stoi(match[2].str());  // 高位数字
            int low = std::stoi(match[3].str());   // 低位数字
            for(int i=low; i<=high; i++) {
                std::string cellname = var_name+"["+std::to_string(i)+"]";
                result.push_back(cellname);
            }
        } else {
            result.push_back(item);
        }
    }
    return result;
}

std::vector<std::string> get_netnames(std::string str) {
    std::vector<std::string> result;
    std::vector<std::string> args = split_to_args(str, true);
    if (args.size()>1) {
        log_assert(args[0] == "list");
        // 多个信号
        args.erase(args.begin());
    }
    // 解析出所有net, 带[]的遍历出来
    std::regex pattern(R"(([a-zA-Z][a-zA-Z0-9_]*)\[(\d+):(\d+)\])");
    std::smatch match;
    for( std::string item: args) {
        if (std::regex_search(item, match, pattern)) {
            std::string var_name = match[1];   // 变量名
            int high = std::stoi(match[2].str());  // 高位数字
            int low = std::stoi(match[3].str());   // 低位数字
            for(int i=low; i<=high; i++) {
                std::string cellname = var_name+"["+std::to_string(i)+"]";
                result.push_back(cellname);
            }
        } else {
            result.push_back(item);
        }
    }
    return result;
}

void Arch::parseXdc(std::istream &in)
{

    if (!in)
        log_error("failed to open Design Constraints file\n");

    std::string line;
    std::string linebuf;
    int lineno = 0; // 读取到第几行，报错时精准定位

    while (std::getline(in, line)) {
        lineno++;
        // 去掉注释部分
        size_t cstart = line.find('#');
        if (cstart != std::string::npos)
            line = line.substr(0, cstart);
        // 去除头尾的空格,多空格替换为单空格
        line = trim_and_reduce_spaces(line);
        if (line.empty())
            continue;
        
        // TODO: 需要用try catch处理
        std::vector<std::string> parse_args = split_to_args(line);
        
        if ( parse_args[0] == "set_property") {
            // 先取最后一个参数，确认目标
            // TODO: 这里只简单解析，嵌套情况后续优化
            std::vector<std::string> obj_args = split_to_args(parse_args[parse_args.size()-1], true);
            // 解析中间的参数
            std::unordered_map<std::string, std::string> param_dict;
            for (size_t arg_index = 1; arg_index < parse_args.size() -1 ; arg_index++) {
                if (parse_args[arg_index] == "-quiet" || parse_args[arg_index] == "-verbose")// 不解析
                    continue;
                else if (parse_args[arg_index] == "-dict") {
                    // 解析dict格式
                    // if (parse_args.size()<arg_index+1)
                    //     log_error("expected an even number of argument for dictionary (on line %d)\n", lineno);
                    std::string dict_content = parse_args[arg_index+1];
                    auto dict_args = split_to_args(dict_content,true);
                    for(size_t i=0; i<dict_args.size(); i+=2) {
                        param_dict[dict_args[i]] = dict_args[i+1];
                    }
                    // param_args.insert(param_args.end(), dict_args.begin(), dict_args.end());
                } else { //  if(parse_args[arg_index] == "PACKAGE_PIN" ||parse_args[arg_index] == "IOSTANDARD") 
                    param_dict[parse_args[arg_index]] = parse_args[arg_index + 1];
                    arg_index++;
                } 
            }
            // 根据目标和参数填充
            if (obj_args[0] == "get_ports" || obj_args[0] == "get_cells") {
                auto cellnames = get_cellnames(obj_args[1]);
                for(auto cellname: cellnames) {
                    IdString name = id(cellname);
                    if (cells.count(name)) {
                        CellInfo * ci = cells.at(name).get();
                        for (const auto& pair : param_dict) {
                            if (pair.first == "LOC" ) {//|| 
                                constrains[ci->name][id("LOC")] = pair.second;
                            } else if (pair.first == "BEL") {
                                constrains[ci->name][id("BEL_TYPE")] = pair.second;
                            } else {
                                ci->attrs[id(pair.first)] = std::string(pair.second);
                            }
                        }
                    }
                }
            } else if (obj_args[0] == "current_design") {
                for (const auto& pair : param_dict) {
                    settings[id(pair.first)] = pair.second;
                }
            }
        } else if (parse_args[0] == "create_clock") {
            // TODO: 这里只简单解析，嵌套情况后续优化
            std::vector<std::string> obj_args = split_to_args(parse_args[parse_args.size()-1], true);
            // 解析中间的参数
            std::unordered_map<std::string, std::string> param_dict;
            double period = -1;
            for (size_t arg_index = 1; arg_index < parse_args.size(); arg_index++) {
                // TODO: 暂不解析-add参数，后续补充
                if (parse_args[arg_index] == "-quiet" || parse_args[arg_index] == "-verbose" || parse_args[arg_index] == "-add")// 不解析
                    continue;
                // TODO: 暂时不支持，后续补充
                else if (parse_args[arg_index] == "-name" || parse_args[arg_index] == "-waveform") {
                    arg_index++;
                    continue;
                } else if (parse_args[arg_index] == "-period") {
                    period = std::stod(parse_args[arg_index+1]);
                    arg_index++;
                }
            }
            if (period<0)
                log_error("create_clock error");
            std::vector<std::string> netnames = get_netnames(obj_args[1]);
            for (auto netname: netnames) {
                IdString name = id(netname);
                NetInfo *maybe_net = getNetByAlias(name);
                if (maybe_net != nullptr) {
                    // TODO: 只考虑create_clock出现一次的情况
                    maybe_net->clkconstr = std::unique_ptr<ClockConstraint>(new ClockConstraint);
                    maybe_net->clkconstr->period = getDelayFromNS(period);
                    maybe_net->clkconstr->high.delay = maybe_net->clkconstr->period.delay / 2;
                    maybe_net->clkconstr->low.delay = maybe_net->clkconstr->period.delay / 2;
                    // period是ns周期，计算出频率hz
                    auto freq = (1000.0 / period) * 1e6 ;
                    settings[id("target_freq")] = std::to_string(freq);
                }
            }
        }
    }
}

NEXTPNR_NAMESPACE_END
