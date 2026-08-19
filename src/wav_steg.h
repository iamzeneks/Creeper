// wav_steg.h — WAV（PCM）无损 LSB 隐写（RIFF 头与 fmt 区零改动）
// 只改 data 区每个样本的最低 depth 位（depth=1：±1 修改，LSB matching；depth=2：±3 以内，
// 低 2 bit 重写）；隐写头（无魔数、无 seed 字段——散布种子由密码派生 crypto_steg_seed(password,
// "creeper-wav")，载荷存在性只能靠"密码正确时 GCM 认证通过"判定）：
//   depth   1B  （承载深度：1 或 2；v1.0 旧格式无此字段，解析失败时自动回退）
//   name_len 2B 大端（原始文件名长度）
//   name     原始文件名 UTF-8（name_len 字节）
//   env_len  4B 大端（信封长度）
// 头+信封体整体按密码派生 seed 用 xorshift64 伪随机散布（纯内容无关序列，
// 占用则线性探测），嵌入后文件可精确重放。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 检测宿主 WAV 是否含可用 password 解出的 creeper 载荷
// （完整解析 + GCM 认证；密码错误或无载荷均返回 false）
bool wav_has_payload(const std::string& path, const std::string& password);

// 嵌入：host_path 载体 + payload_path 载荷 → out_path；
// fill_limit_pct = 填充率上限（百分比，0=仅绝对容量限制；默认 15，超限抛异常）；
// depth = 每样本承载位数（1=原 LSB 方案；2=高容量模式，仅 16-bit 宿主支持）
void wav_embed(const std::string& host_path, const std::string& payload_path,
               const std::string& password, const std::string& out_path,
               int fill_limit_pct = 15, int depth = 1);

// 提取：按隐写头里的原始文件名写出到 out_dir；密码错误/无载荷抛 std::runtime_error
void wav_extract(const std::string& host_path, const std::string& password, const std::string& out_dir);

// —— 分片（split）支持：容量查询 / 原样读流 / 嵌入任意流 ——

// 宿主 100% 填充时的字节容量（样本数×depth bit / 8）
size_t wav_capacity(const std::string& host_path, int depth);

// 读取宿主隐写流（depth 首字节 + head+env 原始字节，不做 GCM 认证）；
// 仅识别新格式（首字节=depth 1..3），旧格式 v1.0 视为无载荷抛异常。
// 密码错误/无载荷抛异常。供分片合并解析使用。
std::vector<uint8_t> wav_read_stream(const std::string& host_path, const std::string& password);

// 嵌入任意隐写流（stream[0] 必须为 depth，调用方已组装好 head+env）；
// fill_limit_pct/depth 语义同 wav_embed
void wav_embed_stream(const std::string& host_path, const std::vector<uint8_t>& stream,
                      const std::string& password, const std::string& out_path,
                      int fill_limit_pct = 15, int depth = 1);
