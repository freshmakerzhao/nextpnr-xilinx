// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2023 Gwenhael Goavec-Merou <gwenhael.goavec-merou@trabucayre.com>
 */

#ifndef SRC_COMMON_HPP_
#define SRC_COMMON_HPP_

#include <sstream>
#include <bitset>
#include <string>
#include <cstdlib>
#include "json.hpp" // 引入 nlohmann/json

// 状态码的枚举类
enum class LevelCode {
    INFO_LOG = 1,                   // 普通信息
    WARNING_LOG = 2,                // 警告
    CRITICAL_LOG = 3,
    ERROR_LOG = 4,                  // 错误
    ALWAYS_LOG = 5,                // 无效
};

// 状态码的枚举类
enum class StatusCode {
    SUCCESS = 200,              // 成功
    BAD_REQUEST = 400,          // 客户端请求错误
    UNAUTHORIZED = 401,         // 未授权
    NOT_FOUND = 404,            // 未找到
    INTERNAL_SERVER_ERROR = 500 // 程序内部错误
};

enum class LogCategory {
    PROJECT,
    CORETCL,
    HYBRDLINK_TCL,
    CONSTRAINTS,
    SYNTHESIS,
    PACK,
    Place,
    ROUTE,
    COMMON,
	IP_FLOW,
	DESIGNUTILS,
	DEVICE,
	NETLIST,
    OPT,
    TIMING
};

// 三种命名管道
enum class PipeType {
    LOG     = 1,                   // 普通信息
    DATA    = 2,                  // 警告
    CONTROL = 3,               // 错误
};

const std::map<LogCategory, int> categoryToInt = {
    {LogCategory::PROJECT, 1},
    {LogCategory::CORETCL, 2},
    {LogCategory::HYBRDLINK_TCL, 4},
    {LogCategory::SYNTHESIS, 8},
	{LogCategory::COMMON, 17},
    {LogCategory::CONSTRAINTS, 18},
    {LogCategory::IP_FLOW, 19},
	{LogCategory::DESIGNUTILS, 20},
    {LogCategory::DEVICE, 21},
    {LogCategory::NETLIST, 22},
    {LogCategory::PACK, 25},
    {LogCategory::Place, 30},
    {LogCategory::OPT, 31},
    {LogCategory::ROUTE, 35},
    {LogCategory::TIMING, 38}
};

// 定义枚举值与字符串的映射
const std::map<LogCategory, std::string> categoryToString = {
    {LogCategory::PROJECT, "Project"},
    {LogCategory::CORETCL, "Coretcl"},
    {LogCategory::HYBRDLINK_TCL, "Hybrdlink_Tcl"},
    {LogCategory::SYNTHESIS, "Synth"},
	{LogCategory::COMMON, "Common"},
    {LogCategory::CONSTRAINTS, "Constraints"},
    {LogCategory::IP_FLOW, "IP_Flow"},
	{LogCategory::DESIGNUTILS, "Designutils"},
    {LogCategory::DEVICE, "Device"},
	{LogCategory::NETLIST, "Netlist"},
    {LogCategory::PACK, "Pack"},
    {LogCategory::Place, "Place"},
    {LogCategory::OPT, "Opt"},
    {LogCategory::ROUTE, "Route"},
    {LogCategory::TIMING, "Timing"}
};

// 获取字符串值
std::string getCategoryToString(LogCategory& category);

// 获取整型值
int getCategoryInt(LogCategory& category);


// 定义 LogData 结构体,用来存储进程通信字段
struct LogData {
    std::string pipe_type;   // 日志前缀
    int level_code;   // 日志前缀
    std::string message_content;   // 日志前缀
    std::string phase; // 日志类别
    std::string sub_phase;  // 日志内容
    std::string category;  // 日志内容
    std::string task_info;  // 日志内容
     // 构造函数
    LogData()
        : pipe_type("log"),
          level_code(static_cast<int>(LevelCode::INFO_LOG)),
          message_content(""),
          phase("IMPLEMENTATION"),
          sub_phase(""),
          category(""),
          task_info("") {}
    // 创建LogData结构体
	static LogData createLogStruct(int levelCode, const std::string& categoryName, int categoryCode, int messageIndex,
								const std::string& taskInfo, const std::string& message) {
		std::string category = "[" + categoryName + " " + std::to_string(categoryCode) + "-" + std::to_string(messageIndex) + "]";
		LogData logData;
		logData.category = category;
		logData.message_content = message;
		logData.pipe_type = "log";
		logData.level_code = levelCode;
		logData.phase = "IMPLEMENTATION";
		logData.sub_phase = "";
		logData.task_info = taskInfo;
		return logData;
	}

};

namespace Common {
    extern std::string _father_process_id; // 父进程id
    extern std::string _log_pipe_name;     // 日志管道名称
    extern std::string _data_pipe_name;    // 数据管道名称
    extern std::string _control_pipe_name; // 控制管道名称
    extern std::string _log_cache; // 日志缓存
	void createLogHeader(std::string& log_info);
    void connectAndSendJson(PipeType pipeType, nlohmann::json jsonData);
    // 动态生成消息唯一标号
    int getNextIndex(const std::string& messagelabel);
	extern std::map<std::string, int> indices;


    /*!
    * \brief 将状态码转换为字符串
    * \param[in] code: 状态码
    * \return 状态码对应的字符串
    */
    std::string statusCodeToString(StatusCode code);

    /*!
    * \brief 构造日志类型的 JSON 数据包
    * \param[in] code: 状态码
    * \param[in] message: 日志消息内容
    * \return 构造好的 JSON 数据包
    */
    // nlohmann::json createLogJson(const int LevelCode,const std::string& categoryName, int categoryCode, int messageIndex,
	//  const std::string& taskInfo, const std::string& message);

    /*!
    * \brief 构造数据类型的 JSON 数据包
    * \param[in] data: 要传输的数据内容
    * \return 构造好的 JSON 数据包
    */
    nlohmann::json createDataJson(StatusCode code, const nlohmann::json& data, const std::string& phase,const std::string& sub_phase);

    /*!
    * \brief 构造控制类型的 JSON 数据包
    * \param[in] command: 控制命令
    * \param[in] parameters: 命令的参数（可选）
    * \return 构造好的 JSON 数据包
    */
    nlohmann::json createControlJson();
}

#endif  // SRC_COMMON_HPP_
