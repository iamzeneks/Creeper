// creeper_cli.cpp — 无界面命令行工具（测试用）
// 用法（参数 UTF-8，内部转 UTF-16 走 Windows API）：
//   creeper_cli seal <in> <out> <password>
//   creeper_cli open <env> <out> <password>
//   creeper_cli embed <host> <payload> <out> <password> [--cap N] [--depth N]
//   creeper_cli extract <host> <outdir> <password>
//   creeper_cli has <host> <password>  # 输出 1/0（无魔数：需密码验证载荷真实性）
// --cap N：PNG/WAV 填充率上限（百分比，默认 15，0=不限制）；MP3 忽略。
// --depth N：仅 WAV，每样本承载位数（1 默认 / 2 高容量，仅 16-bit 宿主；非 WAV 报错）。
// 成功退出码 0；任何失败向 stderr 输出英文错误信息并返回非 0。
#include "crypto.h"
#include "file_util.h"
#include "mp3_steg.h"
#include "png_steg.h"
#include "wav_steg.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int depth_opt = 1; // --depth：仅 WAV（1 默认 / 2 高容量）

std::string lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return r;
}
bool has_ext(const std::string& path, const char* ext) {
    std::string e = lower(path);
    size_t n = std::strlen(ext);
    return e.size() >= n && e.compare(e.size() - n, n, ext) == 0;
}
void usage() {
    fprintf(stderr,
        "usage:\n"
        "  creeper_cli seal <in> <out> <password>\n"
        "  creeper_cli open <env> <out> <password>\n"
        "  creeper_cli embed <host> <payload> <out> <password> [--cap N]\n"
        "  creeper_cli extract <host> <outdir> <password>\n"
        "  creeper_cli has <host> <password>\n");
}

// 解析 "--cap N" / "--depth N"（cap 返回 -1 表示未指定；非法/未知参数抛异常）
int parse_cap(int argc, const std::vector<std::string>& args, int base) {
    int cap = -1;
    for (int i = base; i < argc; i++) {
        if (args[i] == "--cap" && i + 1 < argc) {
            int v = std::atoi(args[i + 1].c_str());
            if (v < 0 || v > 100) throw std::runtime_error("invalid --cap value (0..100)");
            cap = v;
            i++;
        } else if (args[i] == "--depth" && i + 1 < argc) {
            int v = std::atoi(args[i + 1].c_str());
            if (v != 1 && v != 2) throw std::runtime_error("invalid --depth value (1 or 2)");
            depth_opt = v;
            i++;
        } else {
            throw std::runtime_error("unexpected argument: " + args[i]);
        }
    }
    return cap;
}

} // namespace

int main() {
    // 用 CommandLineToArgvW 拿真正的 Unicode 参数，再转 UTF-8（控制台代码页无关）
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) {
        fprintf(stderr, "error: failed to parse command line\n");
        return 1;
    }
    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) args.push_back(utf16_to_utf8(wargv[i]));
    LocalFree(wargv);

    if (argc < 2) {
        usage();
        return 1;
    }
    const std::string& cmd = args[1];
    try {
        if (cmd == "seal" && argc == 5) {
            std::vector<uint8_t> data = read_file_bytes(args[2]);
            std::vector<uint8_t> env = crypto_seal(data, args[4]);
            write_file_bytes(args[3], env);
            return 0;
        }
        if (cmd == "open" && argc == 5) {
            std::vector<uint8_t> env = read_file_bytes(args[2]);
            std::vector<uint8_t> data = crypto_open(env, args[4]);
            write_file_bytes(args[3], data);
            return 0;
        }
        if (cmd == "embed" && argc >= 6) {
            depth_opt = 1;
            int cap = parse_cap(argc, args, 6);
            if (has_ext(args[2], ".png")) {
                if (depth_opt != 1) throw std::runtime_error("--depth only applies to wav hosts");
                png_embed(args[2], args[3], args[5], args[4], cap >= 0 ? cap : 15);
            } else if (has_ext(args[2], ".wav")) {
                wav_embed(args[2], args[3], args[5], args[4], cap >= 0 ? cap : 15, depth_opt);
            } else if (has_ext(args[2], ".mp3")) {
                if (depth_opt != 1) throw std::runtime_error("--depth only applies to wav hosts");
                mp3_embed(args[2], args[3], args[5], args[4]);
            } else throw std::runtime_error("unsupported host file type (need .png, .wav or .mp3)");
            return 0;
        }
        if (cmd == "extract" && argc == 5) {
            if (has_ext(args[2], ".png")) png_extract(args[2], args[4], args[3]);
            else if (has_ext(args[2], ".wav")) wav_extract(args[2], args[4], args[3]);
            else if (has_ext(args[2], ".mp3")) mp3_extract(args[2], args[4], args[3]);
            else throw std::runtime_error("unsupported host file type (need .png, .wav or .mp3)");
            return 0;
        }
        if (cmd == "has" && argc == 4) {
            if (!file_exists(args[2])) throw std::runtime_error("cannot open file: " + args[2]);
            int r = 0;
            if (has_ext(args[2], ".png")) r = png_has_payload(args[2], args[3]) ? 1 : 0;
            else if (has_ext(args[2], ".wav")) r = wav_has_payload(args[2], args[3]) ? 1 : 0;
            else if (has_ext(args[2], ".mp3")) r = mp3_has_payload(args[2], args[3]) ? 1 : 0;
            else throw std::runtime_error("unsupported host file type (need .png, .wav or .mp3)");
            printf("%d\n", r);
            return 0;
        }
        usage();
        return 1;
    } catch (const std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
