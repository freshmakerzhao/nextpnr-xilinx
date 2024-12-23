// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2023 Gwenhael Goavec-Merou <gwenhael.goavec-merou@trabucayre.com>
 */

#include "logger_hybrdlink.h"
#include <windows.h>
#include <iostream>

namespace Common {
    std::string _father_process_id = "-1";
    std::string _log_pipe_name     = R"(\\.\pipe\LogPipe_)";     // 日志管道名称
    std::string _data_pipe_name    = R"(\\.\pipe\DataPipe_)";    // 数据管道名称
    std::string _control_pipe_name = R"(\\.\pipe\ControlPipe_)"; // 控制管道名称
	std::string _log_cache = "";
    /**
     * 拼接log数据
     * @param pipeName: 命名管道名称
     * @param jsonData: 要发送的 JSON 数据包
     */
	void createLogHeader(std::string& log_info) {
		_log_cache += log_info;
	}

    /**
     * 通过命名管道发送 JSON 数据包
     * @param pipeName: 命名管道名称
     * @param jsonData: 要发送的 JSON 数据包
     */
    void connectAndSendJson(PipeType pipeType, nlohmann::json jsonData) {
        if(Common::_father_process_id != "-1"){
            std::string pipeName = Common::_log_pipe_name;
            if (pipeType == PipeType::LOG)
                pipeName = Common::_log_pipe_name + Common::_father_process_id;
            else if (pipeType == PipeType::DATA)
                pipeName = Common::_data_pipe_name + Common::_father_process_id;
            else if (pipeType == PipeType::CONTROL)
                pipeName = Common::_control_pipe_name + Common::_father_process_id;

            // 将 std::string 转换为 std::wstring（Windows 使用宽字符）
            std::wstring wpipeName = std::wstring(pipeName.begin(), pipeName.end());
            // 尝试连接到指定名称的命名管道
            HANDLE hPipe = CreateFileW(
                wpipeName.c_str(),              // 命名管道名称
                GENERIC_WRITE | GENERIC_READ,   // 读写权限
                0,                              // 不共享管道
                NULL,                           // 默认安全属性
                OPEN_EXISTING,                  // 打开现有管道
                0,                              // 不需要异步
                NULL                            // 无模板
            );

            // 处理异常
            if (hPipe == INVALID_HANDLE_VALUE) {
                DWORD errorCode = GetLastError();

                if (errorCode == ERROR_FILE_NOT_FOUND) { // 管道不存在
                     std::cerr << "Pipe not found: " << pipeName << std::endl;
                } else if (errorCode == ERROR_ACCESS_DENIED) { // 访问被拒绝
                     std::cerr << "Access denied to pipe: " << pipeName << std::endl;
                } else {
                     std::cerr << "Failed to connect to pipe: " << pipeName
                             << " Error: " << errorCode << std::endl;
                }
                return;
            }

            // 将 JSON 对象序列化为字符串
			jsonData["message_content"] = _log_cache + jsonData["message_content"].get<std::string>();

			_log_cache = "";
            std::string jsonString = jsonData.dump();

            // 发送 JSON 数据到命名管道
            DWORD bytesWritten = 0;
            if (WriteFile(hPipe, jsonString.c_str(), static_cast<DWORD>(jsonString.length()), &bytesWritten, NULL)) {
                std::cout << "JSON message sent to pipe: "<< pipeName << " (" << std::to_string(bytesWritten) << " bytes written)" << std::endl;
//                printInfo("JSON message sent to pipe: " + pipeName + " (" + std::to_string(bytesWritten) + " bytes written)");
            } else {
                DWORD errorCode = GetLastError();
                std::cerr << "Failed to write to pipe: "<< pipeName << " Error: " << std::to_string(errorCode) << std::endl;
//                printError("Failed to write to pipe: " + pipeName + " Error: " + std::to_string(errorCode));
            }

            // 关闭管道句柄
            // CloseHandle(hPipe);
            // std::cout << "Connection to pipe closed: " << pipeName << std::endl;
//            printInfo("Connection to pipe closed: " + pipeName);
        }
    }

    std::string statusCodeToString(StatusCode code) {
        switch (code) {
            case StatusCode::SUCCESS:
                return "Success";
            case StatusCode::BAD_REQUEST:
                return "Bad Request";
            case StatusCode::UNAUTHORIZED:
                return "Unauthorized";
            case StatusCode::NOT_FOUND:
                return "Not Found";
            case StatusCode::INTERNAL_SERVER_ERROR:
                return "Internal Server Error";
            default:
                return "Unknown Status";
        }
    }

        std::string levelCodeToString(LevelCode code) {
        switch (code) {
            case LevelCode::INFO_LOG:
                return "Info";
            case LevelCode::WARNING_LOG:
                return "Warning";
            case LevelCode::CRITICAL_LOG:
                return "CRITICAL_LOG";
            case LevelCode::ERROR_LOG:
                return "Error";
            case LevelCode::ALWAYS_LOG:
                return "ALWAYS";
            default:
                return "Unknown Log category";
        }
    }

    // {
    //     "type": "log",
    //     "level": "info",
    //     "message": "happy"
//          "phase_info"：{phase:"synth",  sub_phase:"opt_ext"}
    // }
    nlohmann::json createLogJson(StatusCode code, const LogData& data) {
        nlohmann::json packet;
        packet["pipe_type"] = "log";                      // 日志
        packet["level_code"] = static_cast<int>(code);  // 级别
        packet["message_content"] = data.message_content;                 // 内容
        packet["phase"] = data.phase;
        packet["sub_phase"] = data.sub_phase;
        packet["category"] = data.category;
        packet["task_info"] = data.task_info;
        return packet;
    }

    // {
    //     "type": "data",
    //     "status": 200,
    //     "content": ""
    // }
    nlohmann::json createDataJson(StatusCode code, const nlohmann::json& data, const std::string& phase,const std::string& sub_phase) {
        nlohmann::json packet;
        packet["pipe_type"] = "data";      // 数据
        packet["status_code"] = static_cast<int>(code);    // Success 等
        packet["data"] = data;     // 数据内容
		packet["phase"] = phase;
        packet["sub_phase"] = sub_phase;
        return packet;
    }

    // {
    //     "type": "control",
    //     "signal": "progress",
    //     "value": 50
    // }
    nlohmann::json createControlJson() {
        return {};
    }
}
