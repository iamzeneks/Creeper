// audio_app.cpp — 音频伪装转换器（creeper_audio.exe）
// 外观：音频转换专家 v2.8（音频格式转换）；实际：MP3 / WAV 隐写工具。
// Ctrl+Shift+F 呼出隐藏的"ID3 标签"窗，密码藏在"流派"字段。
#include "common_ui.h"
#include "file_util.h"
#include "mp3_steg.h"
#include "wav_steg.h"

#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// 去掉路径与扩展名 → 纯主名
std::string strip_ext(const std::string& in) {
    std::string base = file_basename(in);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    return base;
}

std::string lower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}
std::string host_ext(const std::string& in) {
    std::string e = lower(in);
    size_t dot = e.find_last_of('.');
    return dot == std::string::npos ? "" : e.substr(dot);
}

// 音频伪转换：直接复制字节 + 改后缀（.mp3→.wav 等）
void audio_fake_convert(const std::string& in, const std::string& out_fmt, const std::string& out_dir, std::string& err) {
    try {
        std::vector<uint8_t> bytes = read_file_bytes(in);
        std::string ext = (out_fmt == "MP3") ? "mp3" : "wav";
        std::string out_path = out_dir.empty() ? strip_ext(in) + "_转换." + ext
                                               : out_dir + "/" + strip_ext(in) + "_转换." + ext;
        write_file_bytes(out_path, bytes);
    } catch (const std::exception& e) {
        err = e.what();
    }
}

void audio_extract(const std::string& host, const std::string& out_dir, const std::string& pwd, std::string& err) {
    try {
        if (host_ext(host) == ".wav") wav_extract(host, pwd, out_dir);
        else mp3_extract(host, pwd, out_dir);
    } catch (const std::exception& e) { err = e.what(); }
}

} // namespace

namespace {
// 窗口类名（XOR 0x55 混淆，"CreeperAudioApp"）
const uint8_t kClassXor[] = {0x16,0x27,0x30,0x30,0x25,0x30,0x27,0x14,0x20,0x31,0x3C,0x3A,0x14,0x25,0x25};
std::string xstr(const uint8_t* x, size_t n) {
    std::string s;
    s.reserve(n);
    for (size_t i = 0; i < n; i++) s.push_back((char)(x[i] ^ 0x55));
    return s;
}
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UIRuntime rt;
    rt.win_class = xstr(kClassXor, sizeof(kClassXor));
    rt.app_title = "音频转换专家 v2.8";
    rt.hidden_title = "ID3 标签";
    rt.is_image = false;
    rt.formats = {"MP3", "WAV"};
    rt.extract_fn = audio_extract;
    rt.fake_convert_fn = audio_fake_convert;
    return ui_run(rt);
}
