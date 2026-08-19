// common_ui.h — 共享 GUI 基建（DX11 窗口 + ImGui + 中文字体 + 快捷键 + 文件对话框 + UTF 转换）
// 两个 exe（creeper_img / creeper_audio）共用：伪格式转换主界面 + Ctrl+Shift+F 隐藏元数据窗。
#pragma once

#include <string>
#include <vector>

// 应用运行时配置（img_app / audio_app 填充后交给 ui_run）
struct UIRuntime {
    std::string win_class;      // Win32 窗口类名（两个 exe 各自独立）
    std::string app_title;      // 主窗口/主界面标题（如"格式转换大师 v3.2"）
    std::string hidden_title;   // 隐藏元数据窗标题（如"EXIF 信息"）
    bool is_image = true;       // true=图片（EXIF 字段），false=音频（ID3 字段）
    std::vector<std::string> formats; // 输出格式下拉选项（JPG/PNG 或 MP3/WAV）

    // 由 app 注入的回调；失败信息一律写入 err（英文）
    void (*extract_fn)(const std::string& host, const std::string& out_dir,
                       const std::string& pwd, std::string& err) = nullptr;
    void (*fake_convert_fn)(const std::string& in, const std::string& out_fmt,
                            const std::string& out_dir, std::string& err) = nullptr;

    // —— 运行时状态（common_ui 内部维护）——
    std::vector<std::string> files;   // 文件列表（可多选/拖拽）
    std::vector<char> selected;       // 多选状态（与 files 对齐）
    int fmt_idx = 0;                  // 输出格式选中项
    std::string out_dir;              // 输出目录
    std::string password;             // 内存中的"密码"（由隐藏窗确定按钮写入）
    int cap_pct = 15;                 // 嵌入填充率上限 %（隐藏窗"编码质量"映射，15/30/50/100）
    int depth = 1;                    // WAV 承载深度（隐藏窗音频"位深"映射，1/2/3；仅 audio 生效）
    bool hw_accel = false;            // 「硬件加速」勾选（主界面）：勾选=加密嵌入，不勾选=解密提取
    std::vector<std::string> pending_drops; // WM_DROPFILES 收集的拖拽文件
    std::string status;               // 底部状态栏文本
    std::string modal_msg;            // 弹窗文案
    bool modal = false;               // 需要弹窗
    bool hidden_open = false;         // 隐藏元数据窗是否打开
    float progress = 0.0f;            // 转换进度条 0.0~1.0
};

// 阻塞进入主循环，返回进程退出码
int ui_run(UIRuntime& rt);
