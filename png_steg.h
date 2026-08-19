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

#include <string>

// 检测宿主 PNG 是否含可用 password 解出的 creeper 载荷
// （完整解析 + GCM 认证；密码错误或无载荷均返回 false）
bool png_has_payload(const std::string& path, const std::string& password);

// 嵌入：host_path 载体 + payload_path 载荷 → out_path；
// fill_limit_pct = 填充率上限（百分比，0=仅绝对容量限制；默认 15，超限抛异常）
void png_embed(const std::string& host_path, const std::string& payload_path,
               const std::string& password, const std::string& out_path, int fill_limit_pct = 15);

// 提取：按隐写头里的原始文件名写出到 out_dir；密码错误/无载荷抛 std::runtime_error
void png_extract(const std::string& host_path, const std::string& password, const std::string& out_dir);