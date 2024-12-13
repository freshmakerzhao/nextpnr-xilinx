
#ifndef ARCHIVETOOL_H
#define ARCHIVETOOL_H

// #include "vector"

namespace Tool{

    using byte_t = unsigned char;
    class ArchiveTool{
        public:
            // ArchiveTool();
            /**
             * @description:                      从给定路径的压缩文件中提取文件内容到缓冲区
             * @param {string&} inArchive         压缩文件路径
             * @param {vector<byte_t>&} outBuffer 存储文件数据
             * @return {*}                        将outBuffer返回
             */
            std::vector<byte_t> extract(const std::string& inArchive,std::vector<byte_t>& outBuffer);
            /**
             * @description:                      使用密码从给定路径的压缩文件中提取文件内容到缓冲区
             * @param {string&} inArchive         压缩文件路径
             * @param {vector<byte_t>&} outBuffer 存储文件数据
             * @param {string} mPassword          密码值
             * @return {*}                        将outBuffer返回
             */
            std::vector<byte_t> extractWithPassword(const std::string& inArchive,std::vector<byte_t>& outBuffer,std::string mPassword);
            /**
             * @description:                      将缓冲区的数据输出至压缩格式的文件
             * @param {vector<byte_t>&} outBuffer 想要压缩的数据
             * @param {string&} outFile           压缩文件的输出路径
             * @param {string&} fileName          压缩文件内部的文件名（包括后缀）
             * @return {*}
             */
            void compress(const std::vector<byte_t>& buffer, const std::string& outFile,const std::string& fileName);
            /**
             * @description:                      使用密码将缓冲区的数据输出至压缩格式的文件
             * @param {vector<byte_t>&} outBuffer 想要压缩的数据
             * @param {string&} outFile           压缩文件的输出路径
             * @param {string&} fileName          压缩文件内部的文件名（包括后缀）
             * @param {string} mPassword          密码值
             * @return {*}
             */
            void compressWithPassword(const std::vector<byte_t>& buffer, const std::string& outFile,const std::string& fileName,std::string mPassword);
            /**
             * @description:                      使用密码将缓冲区的数据输出至压缩格式的文件
             * @param {vector<byte_t>&} outBuffer 想要压缩的数据
             * @param {ostream&} outStream        输出流
             * @param {string&} fileName          压缩文件内部的文件名（包括后缀）
             * @param {string} mPassword          密码值
             * @return {*}
             */
            void compressWithPassword(const std::vector<byte_t>& buffer, std::ostream& outStream,const std::string& fileName,std::string mPassword);
            /**
             * @description:                      将string数据写入buffer中
             * @param {string&} data              自定义数据
             * @param {vector<byte_t>&} outBuffer 存储自定义数据的缓冲区
             * @return {*}
             */
            std::vector<byte_t> string_to_byte(const std::string& data,std::vector<byte_t>& buffer);
            /**
             * @description:                      将buffer数据转换为string
             * @param {vector<byte_t>&} outBuffer 想要转换的数据内容
             * @return {*}
             */
            std::string byte_to_string(const std::vector<byte_t>& buffer);
    };
}

#endif 
// MYTOOLS_HPP