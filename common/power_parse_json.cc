#include "power_parse_json.h"
#include <fstream>
#include <iostream>
#include <sstream>
// 构造函数
PowerJsonReader::PowerJsonReader(const std::string &filePath) : filePath(filePath){}

// 设置文件路径
void PowerJsonReader::SetFilePath(const std::string &filePath) {
    this->filePath = filePath;
}

// 加载 JSON 数据到项目数据结构
bool PowerJsonReader::LoadData(json &json_data) {
    // 检查文件路径是否为空
    if (filePath.empty()) {
        std::cerr << "Error: File path is empty." << std::endl;
        return false;
    }

    // 打开文件
    std::ifstream inFile(filePath);
    if (!inFile.is_open()) {
        std::cerr << "Error: Failed to open file: " << filePath << std::endl;
        return false;
    }
    // 解析 JSON 文件
    std::ostringstream oss;
    oss << inFile.rdbuf();
    inFile.close();  // 关闭文件
    std::string err;
    json_data = json::parse(oss.str(), err);
    // 检查解析是否成功
    if (!err.empty()) {
        std::cerr << "JSON 解析错误: " << err << std::endl;
        return false;
    }
    // std::ofstream out_file("C:/msys64/home/DELL/Source/my_nextpnr-xilinx/power_data/test_parse.json");
    // if (!out_file.is_open()) {
    //     std::cerr << "Failed to open output file for writing." << std::endl;
    //     return false;
    // }
    // out_file << json_data.dump() << std::endl;
    // out_file.close();
    // 成功读取数据
    return true;
}