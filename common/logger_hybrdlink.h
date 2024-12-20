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
enum class level_code {
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

// 三种命名管道
enum class PipeType {
    LOG     = 1,                   // 普通信息
    DATA    = 2,                  // 警告
    CONTROL = 3,               // 错误
};


namespace Common {
    extern std::string _father_process_id; // 父进程id
    extern std::string _log_pipe_name;     // 日志管道名称
    extern std::string _data_pipe_name;    // 数据管道名称
    extern std::string _control_pipe_name; // 控制管道名称
    extern std::string _log_cache; // 日志缓存
	void createLogHeader(std::string& log_info);
    void connectAndSendJson(PipeType pipeType, nlohmann::json jsonData);

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
    nlohmann::json createLogJson(StatusCode code, const std::string& message, const std::string& phase,const std::string& sub_phase,const std::string& category);

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
