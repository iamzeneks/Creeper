// crypto.h — 加密信封（内置纯软件 AES-256-GCM + PBKDF2-HMAC-SHA256，无系统加密库调用）
// 信封格式与 envelope.py 字节级兼容：
//   偏移 0   8B  magic "CREEPER1"
//   偏移 8   1B  version 0x01
//   偏移 9   16B salt（随机）
//   偏移 25  12B nonce（随机）
//   偏移 37  4B  ct_len（大端，密文长度，含 16B GCM tag）
//   偏移 41  ct_len 字节密文（ciphertext || tag）
// seal 前先做 DEFLATE 压缩（内置实现，输出标准 RFC1950 zlib 流，Python zlib 可直接解）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 信封头常量
constexpr size_t CRYPTO_SALT_LEN = 16;    // PBKDF2 盐长度
constexpr size_t CRYPTO_NONCE_LEN = 12;   // GCM nonce 长度
constexpr size_t CRYPTO_TAG_LEN = 16;     // GCM 认证 tag 长度
constexpr size_t CRYPTO_HEADER_LEN = 8 + 1 + CRYPTO_SALT_LEN + CRYPTO_NONCE_LEN + 4; // 41

// 加密任意字节 → 信封字节（压缩 → AES-256-GCM）
std::vector<uint8_t> crypto_seal(const std::vector<uint8_t>& payload, const std::string& password);

// 信封 → 原始字节；密码错误或数据被篡改时抛 std::runtime_error
std::vector<uint8_t> crypto_open(const std::vector<uint8_t>& envelope, const std::string& password);

// 隐写散布种子派生：HMAC-SHA256(password, tag) 前 4 字节。
// 轻量派生（seed 非机密，机密性由 GCM 保证）；用途 = 散布/keystream 位置随
// 密码变化，消除隐写头/位流的明文固定结构。
uint32_t crypto_steg_seed(const std::string& password, const std::string& tag);
