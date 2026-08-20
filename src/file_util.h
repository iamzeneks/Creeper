// file_util.h — 跨文件共享的路径/文件读写/安全随机小工具（头文件内联实现）
// 所有对外接口一律使用 UTF-8 路径。
// Windows：内部走 Windows API 的 UTF-16；POSIX（Linux/macOS）：路径本就是 UTF-8，
// 直接使用标准库，utf8/utf16 转换仅作为 API 兼容占位（调用方已用 #ifdef 隔离）。
#pragma once

#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#ifdef _WIN32
// UTF-8 → UTF-16（用于 Windows API）
inline std::wstring utf8_to_utf16(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) throw std::runtime_error("invalid utf-8 string");
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// UTF-16 → UTF-8
inline std::string utf16_to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) throw std::runtime_error("invalid utf-16 string");
    std::string s((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
#else
// POSIX 无 UTF-16 概念；调用方已隔离，这里仅做失败占位。
inline std::wstring utf8_to_utf16(const std::string&) {
    throw std::runtime_error("utf16 conversion is not supported on this platform");
}
inline std::string utf16_to_utf8(const std::wstring&) {
    throw std::runtime_error("utf16 conversion is not supported on this platform");
}
#endif

// 读取整个文件（二进制）；失败抛 std::runtime_error
inline std::vector<uint8_t> read_file_bytes(const std::string& path) {
#ifdef _WIN32
    FILE* f = _wfopen(utf8_to_utf16(path).c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) throw std::runtime_error("cannot open file: " + path);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(sz > 0 ? (size_t)sz : 0);
    if (sz > 0 && fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        throw std::runtime_error("read failed: " + path);
    }
    fclose(f);
    return buf;
}

// 写入整个文件（二进制）；失败抛 std::runtime_error
inline void write_file_bytes(const std::string& path, const std::vector<uint8_t>& data) {
#ifdef _WIN32
    FILE* f = _wfopen(utf8_to_utf16(path).c_str(), L"wb");
#else
    FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) throw std::runtime_error("cannot write file: " + path);
    if (!data.empty() && fwrite(data.data(), 1, data.size(), f) != data.size()) {
        fclose(f);
        throw std::runtime_error("write failed: " + path);
    }
    fclose(f);
}

// 路径 → 纯文件名（去掉目录部分）
inline std::string file_basename(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

// 文件是否存在
inline bool file_exists(const std::string& path) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesW(utf8_to_utf16(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0;
#endif
}

// 递归创建目录（UTF-8 路径）；已存在或失败时静默忽略
inline void create_dirs(const std::string& path) {
#ifdef _WIN32
    std::wstring w = utf8_to_utf16(path);
    std::wstring cur;
    for (size_t i = 0; i < w.size(); i++) {
        wchar_t c = w[i];
        cur.push_back(c);
        if (c == L'/' || c == L'\\') {
            if (!cur.empty()) CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    if (!cur.empty()) CreateDirectoryW(cur.c_str(), nullptr);
#else
    std::string cur;
    for (char c : path) {
        cur.push_back(c);
        if (c == '/') {
            if (!cur.empty()) ::mkdir(cur.c_str(), 0755);
        }
    }
    if (!cur.empty()) ::mkdir(cur.c_str(), 0755);
#endif
}

// 安全随机字节：std::random_device 系统 CSPRNG（Windows→RtlGenRandom / Linux→getrandom
// 或 /dev/urandom / macOS→arc4random），跨平台安全且不引入额外导入表。
// 注意：此函数构造 random_device，只应在低频处调用（如一次性种子/加密盐/nonce）。
inline void secure_random_bytes(uint8_t* out, size_t n) {
    std::random_device rd;
    size_t i = 0;
    while (i < n) {
        uint32_t v = rd();
        size_t take = (n - i) < sizeof(v) ? (n - i) : sizeof(v);
        for (size_t j = 0; j < take; j++) out[i + j] = (uint8_t)((v >> (8 * j)) & 0xFF);
        i += take;
    }
}