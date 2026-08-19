// png_steg.h — PNG 隐写（stb_image 解码 / stb_image_write 无损回写）
// 只嵌入 RGB 三通道各 1 bit；若原图带 alpha 则按 RGBA 载入，alpha 保持原样。
// 嵌入用 ±1（LSB matching）+ 未承载像素直方图配对补偿（抗统计检测）。
// 隐写头（无魔数、无 seed 字段——散布种子由密码派生 crypto_steg_seed(password,
// "creeper-seed")，载荷存在性只能靠"密码正确时 GCM 认证通过"判定，静态扫描
// 无可命中特征）：
//   name_len 2B  大端（原始文件名长度）
//   name     原始文件名 UTF-8（name_len 字节）
//   env_len  4B  大端（信封长度）
// 头+信封体整体按密码派生 seed 用 xorshift64 伪随机散布（纯内容无关序列，
// 占用则线性探测），嵌入后图可精确重放。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 检测宿主 PNG 是否含可用 password 解出的 creeper 载荷
// （完整解析 + GCM 认证；密码错误或无载荷均返回 false）
bool png_has_payload(const std::string& path, const std::string& password);

// 嵌入：host_path 载体 + payload_path 载荷 → out_path；
// fill_limit_pct = 填充率上限（百分比，0=仅绝对容量限制；默认 15，超限抛异常）
void png_embed(const std::string& host_path, const std::string& payload_path,
               const std::string& password, const std::string& out_path, int fill_limit_pct = 15);

// 提取：按隐写头里的原始文件名写出到 out_dir；密码错误/无载荷抛 std::runtime_error
void png_extract(const std::string& host_path, const std::string& password, const std::string& out_dir);

// —— 分片（split）支持：容量查询 / 原样读流 / 嵌入任意流 ——

// 宿主 100% 填充时的字节容量（像素数×3 bit / 8）
size_t png_capacity(const std::string& host_path);

// 读取宿主隐写流（head+env 原始字节，不做 GCM 认证）；密码错误/无载荷抛异常。
// 供分片合并解析使用（单块认证必然失败，需先按原样收集再拼接）。
std::vector<uint8_t> png_read_stream(const std::string& host_path, const std::string& password);

// 嵌入任意隐写流（调用方已组装好 head+env）；fill_limit_pct 语义同 png_embed
void png_embed_stream(const std::string& host_path, const std::vector<uint8_t>& stream,
                      const std::string& password, const std::string& out_path, int fill_limit_pct = 15);