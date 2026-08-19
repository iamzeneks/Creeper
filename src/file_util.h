// file_util.h — 跨文件共享的 UTF-8/UTF-16 转换与二进制文件读写小工具（头文件内联实现）
// 所有对外接口一律使用 UTF-8 路径；内部走 Windows API 的 UTF-16。
#pragma once

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

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

// 读取整个文件（二进制）；失败抛 std::runtime_error
inline std::vector<uint8_t> read_file_bytes(const std::string& path) {
    FILE* f = _wfopen(utf8_to_utf16(path).c_str(), L"rb");
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
    FILE* f = _wfopen(utf8_to_utf16(path).c_str(), L"wb");
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
    DWORD attrs = GetFileAttributesW(utf8_to_utf16(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
}

// 递归创建目录（UTF-8 路径）；已存在或失败时静默忽略（CreateDirectoryW 返回 FALSE 不抛）
inline void create_dirs(const std::string& path) {
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
}
