// img_app.cpp — 图片伪装转换器（creeper_img.exe）
// 外观：格式转换大师 v3.2（图片格式转换）；实际：PNG 隐写工具。
// Ctrl+Shift+F 呼出隐藏的"EXIF 信息"窗，密码藏在"镜头格式"字段。
#include "common_ui.h"
#include "file_util.h"
#include "png_steg.h"
#include "split_steg.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// 仅声明 stb 接口（实现单元在 png_steg.cpp）
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

namespace {

// stbi_write 回调：写入内存缓冲
void stbi_write_cb(void* context, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(context);
    auto* p = static_cast<const uint8_t*>(data);
    v->insert(v->end(), p, p + size);
}

// 去掉路径与扩展名 → 纯主名
std::string strip_ext(const std::string& in) {
    std::string base = file_basename(in);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    return base;
}

// 图片伪转换：stb 真实格式转换（PNG↔JPG）
void img_fake_convert(const std::string& in, const std::string& out_fmt, const std::string& out_dir, std::string& err) {
    try {
        std::vector<uint8_t> bytes = read_file_bytes(in);
        int w = 0, h = 0, comp = 0;
        unsigned char* img = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &comp, 4);
        if (!img) {
            err = "cannot decode image";
            return;
        }
        std::string ext = (out_fmt == "JPG") ? "jpg" : "png";
        std::string out_path = out_dir.empty() ? strip_ext(in) + "_转换." + ext
                                               : out_dir + "/" + strip_ext(in) + "_转换." + ext;
        std::vector<uint8_t> out_buf;
        int ok;
        if (out_fmt == "JPG")
            ok = stbi_write_jpg_to_func(stbi_write_cb, &out_buf, w, h, 4, img, 90);
        else
            ok = stbi_write_png_to_func(stbi_write_cb, &out_buf, w, h, 4, img, w * 4);
        stbi_image_free(img);
        if (!ok) {
            err = "failed to write output image";
            return;
        }
        write_file_bytes(out_path, out_buf);
    } catch (const std::exception& e) {
        err = e.what();
    }
}

void img_extract(const std::string& host, const std::string& out_dir, const std::string& pwd, std::string& err) {
    // GUI 嵌入一律走 split_embed（2 文件单宿主产物也是分片块格式），单文件提取
    // 先按分片还原；失败再回退普通 png 信封；都不行才报错 → 回退伪转换。
    try {
        split_extract({host}, pwd, out_dir);
        return;
    } catch (const std::exception&) {
    }
    try { png_extract(host, pwd, out_dir); } catch (const std::exception& e) { err = e.what(); }
}

} // namespace

namespace {
// 窗口类名（XOR 0x55 混淆，"CreeperImgApp"）
const uint8_t kClassXor[] = {0x16,0x27,0x30,0x30,0x25,0x30,0x27,0x1C,0x38,0x32,0x14,0x25,0x25};
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
    rt.app_title = "格式转换大师 v3.2";
    rt.hidden_title = "EXIF 信息";
    rt.is_image = true;
    rt.formats = {"JPG", "PNG"};
    rt.extract_fn = img_extract;
    rt.fake_convert_fn = img_fake_convert;
    return ui_run(rt);
}
