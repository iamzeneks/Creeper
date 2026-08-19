// mp3_steg.h — MP3 帧头辅助位隐写（音频数据零改动）
// 只改写每个 MPEG 音频帧头的 3 个辅助位（private/copyright/original，每帧 3 bit），
// 位流按帧排列（private=最高位, copyright, original=最低位）。
// 位流布局（无魔数——载荷存在性靠"密码正确时 GCM 认证通过"判定）：
//   name_len(16bit) | name(8bit×n) | env_len(32bit) | env(8bit×m)，末尾填充 0 位到 3 的倍数
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 检测宿主 MP3 是否含可用 password 解出的 creeper 载荷
// （完整解析 + GCM 认证；密码错误或无载荷均返回 false）
bool mp3_has_payload(const std::string& path, const std::string& password);

// 嵌入：host_path 载体 + payload_path 载荷 → out_path
void mp3_embed(const std::string& host_path, const std::string& payload_path,
               const std::string& password, const std::string& out_path);

// 提取：按位流里的原始文件名写出到 out_dir；密码错误/无载荷抛 std::runtime_error
void mp3_extract(const std::string& host_path, const std::string& password, const std::string& out_dir);

// —— 分片（split）支持：容量查询 / 原样读流 / 嵌入任意流 ——

// 宿主 100% 填充时的字节容量（帧数×3 bit / 8）
size_t mp3_capacity(const std::string& host_path);

// 读取宿主隐写流（name_len|name|env_len|env 原始字节，不含尾部填充位，不做 GCM 认证）；
// 密码错误/无载荷抛异常。供分片合并解析使用。
std::vector<uint8_t> mp3_read_stream(const std::string& host_path, const std::string& password);

// 嵌入任意隐写流（调用方已组装好 name_len|name|env_len|env，末尾自动补 0 位到 3 的倍数）
void mp3_embed_stream(const std::string& host_path, const std::vector<uint8_t>& stream,
                      const std::string& password, const std::string& out_path);