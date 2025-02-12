#ifndef POWER_PARSE_JSON_H
#define POWER_PARSE_JSON_H

#include <string>
#include <vector>
#include <map>
#include "json11.hpp"

using json = json11::Json;
// JsonReader 类定义
class PowerJsonReader {
public:
    // 构造函数和析构函数
    PowerJsonReader() = default;
    PowerJsonReader(const std::string &filePath);
    ~PowerJsonReader() = default;

    // 设置文件路径
    void SetFilePath(const std::string &filePath);

    // 加载 JSON 数据到项目结构
    bool LoadData(json &json_data);

private:
    std::string filePath;  // JSON 文件路径
};

#endif