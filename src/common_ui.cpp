// common_ui.cpp — 共享 GUI 基建实现
// 窗口：DX11 + ImGui（暗色主题 + 中文字体）；主界面为"格式转换器"伪装，
// Ctrl+Shift+F 呼出隐藏的元数据编辑窗（EXIF/ID3 伪字段，密码藏在"镜头格式"/"流派"）。
#include "common_ui.h"

#include "file_util.h"
#include "split_steg.h"

#include <windows.h>
#include <d3d11.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// imgui 1.92+ 将 WndProcHandler 声明注释在 #if 0 块中，官方要求使用方自行前向声明
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------- DX11 全局 ----------
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static bool g_ResizePending = false;
static int g_ResizeWidth = 0, g_ResizeHeight = 0;
static HWND g_hwnd = nullptr;
static UIRuntime* g_rt = nullptr;

// ---------- 转换工作线程状态 ----------
static std::atomic<bool> g_busy{false};
static std::atomic<bool> g_done{false};
static std::atomic<bool> g_success{false};
static std::atomic<float> g_progress{0.0f};
static std::mutex g_err_mu;
static std::string g_err;
static std::thread g_worker;

// ---------- 隐藏元数据窗字段缓冲 ----------
static struct {
    // 图片（EXIF）
    char manufacturer[64];
    char model[64];
    char lens[64];
    char pwd[128]; // "镜头格式" ← 密码
    char aperture[16];
    char shutter[16];
    char iso[16];
    char wb[16];
    char date[32];
    char resolution[16];
    char software[64];
    char note[128];
    // 音频（ID3）
    char title[64];
    char artist[64];
    char album[64];
    char year[16];
    char genre[128]; // "流派" ← 密码
    char track[16];
    char bitrate[16];
    char sample_rate[16];
    char comment[128];
    int quality = 0; // "编码质量"：0=标准 15% / 1=高 30% / 2=超高 50% / 3=极限 100%
    int bitdepth = 0; // "位深"（audio 窗）：0=标准 16bit → depth 1 / 1=高 24bit → depth 2 / 2=超清 32bit → depth 3
    bool pwd_touched = false;   // 密码字段（镜头格式/流派）是否被用户编辑过
    bool genre_touched = false; // 未编辑过 → 确定时视为空密码（默认假数据不当密码用）
} g_meta;

// 密码字段编辑回调：用户一旦改动（含清空/重填）即置 touched，
// 此后确定才把字段内容当真密码。
static int meta_pwd_cb(ImGuiInputTextCallbackData* data) {
    (void)data;
    if (g_rt && g_rt->is_image) g_meta.pwd_touched = true;
    else g_meta.genre_touched = true;
    return 0;
}

// ---------- 关于页（原生模态对话框）/ 自毁 ----------
static bool g_self_destruct = false;

// 原生「关于」对话框：独立 Win32 弹窗（WS_EX_DLGMODALFRAME），紧凑固定尺寸，
// 与主窗口完全分离（真正弹出，非 ImGui 内嵌窗口）；控件用系统字体 + 手动 WM_COMMAND。
namespace {
constexpr UINT IDC_ABOUT_CLOSE = 1001;
constexpr UINT IDC_ABOUT_SITE = 1002;
}

static LRESULT CALLBACK about_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        UINT dpi = GetDpiForSystem();
        float s = dpi ? (float)dpi / 96.0f : 1.0f;
        auto mkfont = [s](int px, bool bold) {
            return CreateFontW((int)(px * s + 0.5f), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Microsoft YaHei");
        };
        HFONT f_title = mkfont(20, true), f_body = mkfont(16, false), f_small = mkfont(13, false);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)f_title); // 只记首字体，WM_DESTROY 统一释放
        UIRuntime* rt = g_rt;
        auto mkstatic = [&](int y, int fh, const wchar_t* txt, HFONT f, bool gray) {
            HWND c = CreateWindowExW(0, L"STATIC", txt, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     20, y, 360, fh, h, nullptr, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
            if (gray) SetPropW(c, L"gray", (HANDLE)1);
            return c;
        };
        mkstatic(14, 30, utf8_to_utf16(rt->app_title).c_str(), f_title, false);
        mkstatic(50, 24, L"北京星辉数媒科技有限公司", f_body, false);
        mkstatic(78, 18, L"Copyright (C) 2024-2026 Beijing Xinghui Digital Media Co., Ltd.", f_small, true);
        mkstatic(98, 18, L"All rights reserved.", f_small, true);
        mkstatic(122, 34, L"本软件仅用于图片/音频格式转换，不含任何附加功能。", f_small, true);
        HWND b_close = CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                       196, 186, 92, 32, h, (HMENU)IDC_ABOUT_CLOSE,
                                       GetModuleHandleW(nullptr), nullptr);
        HWND b_site = CreateWindowExW(0, L"BUTTON", L"访问官网", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      296, 186, 84, 32, h, (HMENU)IDC_ABOUT_SITE,
                                      GetModuleHandleW(nullptr), nullptr);
        SendMessageW(b_close, WM_SETFONT, (WPARAM)f_body, TRUE);
        SendMessageW(b_site, WM_SETFONT, (WPARAM)f_body, TRUE);
        return 0;
    }
    case WM_COMMAND: {
        switch (LOWORD(wp)) {
        case IDC_ABOUT_CLOSE:
            DestroyWindow(h);
            return 0;
        case IDC_ABOUT_SITE: {
            DestroyWindow(h);
            UIRuntime* rt = g_rt;
            int r = MessageBoxW(g_hwnd,
                                L"即将访问外部网站：\nwww.xinghui-multimedia.cn\n\n确定要继续吗？",
                                utf8_to_utf16(rt->app_title).c_str(),
                                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
            if (r == IDYES) {
                g_self_destruct = true; // 核按钮：访问官网 = 销毁本程序
                PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        HWND c = (HWND)lp;
        if (GetPropW(c, L"gray")) {
            SetTextColor(dc, RGB(0x80, 0x80, 0x80));
        } else {
            SetTextColor(dc, RGB(0x20, 0x20, 0x20));
        }
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_DESTROY: {
        HFONT f = (HFONT)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (f) DeleteObject(f);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// 同步模态循环：弹窗期间主窗禁用（真模态），WM_QUIT 重新投递避免丢失
static void show_about_dialog() {
    static bool class_ready = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!class_ready) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.hInstance = inst;
        wc.lpfnWndProc = about_proc;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"XhAboutDlg";
        RegisterClassExW(&wc); // 失败 = 已注册（class_ready 之外），复用即可
        class_ready = true;
    }
    UINT dpi = GetDpiForSystem();
    float s = dpi ? (float)dpi / 96.0f : 1.0f;
    UIRuntime* rt = g_rt;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"XhAboutDlg",
                               utf8_to_utf16(rt->app_title + " - 关于").c_str(),
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               (int)(400 * s + 0.5f), (int)(254 * s + 0.5f),
                               g_hwnd, nullptr, inst, nullptr);
    if (!dlg) return;
    EnableWindow(g_hwnd, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage((int)msg.wParam);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);
}

// 抹掉内存中的密码等敏感内容（退出与自毁时调用）
static void wipe_secrets(UIRuntime& rt) {
    volatile uint8_t* m = reinterpret_cast<volatile uint8_t*>(&g_meta);
    for (size_t i = 0; i < sizeof(g_meta); i++) m[i] = 0;
    if (!rt.password.empty()) {
        volatile char* p = reinterpret_cast<volatile char*>(rt.password.data());
        for (size_t i = 0; i < rt.password.size(); i++) p[i] = 0;
        rt.password.clear();
    }
}

// 自毁：删除自身 exe。运行中的 exe 无法直接删除（映像句柄无 FILE_SHARE_DELETE），
// 因此写入临时 bat 延迟自删（进程退出后执行）；路径一律转 8.3 短路径（纯 ASCII），
// 避免 bat 按控制台代码页解析中文路径时乱码
namespace {
// "msimg32_upd.bat"：自删脚本的无害化文件名（XOR 0x55 混淆；运行时也不出现
// creeper 字样——%TEMP% 里出现 creeper 文件名本身就是暴露）
const uint8_t kSelfdelBatXor[] = {0x38,0x26,0x3C,0x38,0x32,0x66,0x67,0x0A,0x20,0x25,0x31,0x7B,0x37,0x34,0x21};
std::string xstr(const uint8_t* x, size_t n) {
    std::string s;
    s.reserve(n);
    for (size_t i = 0; i < n; i++) s.push_back((char)(x[i] ^ 0x55));
    return s;
}
}
static void self_destruct_exe() {
    wchar_t self[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring bat = std::wstring(tmp) + utf8_to_utf16(xstr(kSelfdelBatXor, sizeof(kSelfdelBatXor)));
    // 先落盘占位（GetShortPathNameW 需要文件存在）
    FILE* f = _wfopen(bat.c_str(), L"wb");
    if (!f) return;
    fwrite("@echo off\r\n", 1, 11, f);
    fclose(f);
    wchar_t short_self[MAX_PATH] = {0};
    wchar_t short_bat[MAX_PATH] = {0};
    GetShortPathNameW(self, short_self, MAX_PATH);
    GetShortPathNameW(bat.c_str(), short_bat, MAX_PATH);
    if (!short_self[0] || !short_bat[0]) return;
    std::string script = "@echo off\r\nping 127.0.0.1 -n 3 >nul\r\ndel /f /q \"" +
                         utf16_to_utf8(short_self) + "\"\r\ndel /f /q \"" +
                         utf16_to_utf8(short_bat) + "\"\r\n";
    f = _wfopen(bat.c_str(), L"wb");
    if (!f) return;
    fwrite(script.data(), 1, script.size(), f);
    fclose(f);
    ShellExecuteW(nullptr, L"open", bat.c_str(), nullptr, nullptr, SW_HIDE);
}

// 每次打开隐藏窗时重置为预制值（确定按钮不真正修改任何文件，纯障眼）。
// 密码字段（镜头格式/流派）也填默认假数据，避免"就它一个空着"露馅；
// 未编辑过（touched=false）时确定一律视为空密码——假数据绝不会被当真密码用。
static void fill_presets(UIRuntime& rt) {
    std::memset(&g_meta, 0, sizeof(g_meta));
    if (rt.is_image) {
        std::strncpy(g_meta.manufacturer, "Canon", sizeof(g_meta.manufacturer) - 1);
        std::strncpy(g_meta.model, "Canon EOS R6 Mark II", sizeof(g_meta.model) - 1);
        std::strncpy(g_meta.lens, "RF24-70mm F2.8 L IS USM", sizeof(g_meta.lens) - 1);
        std::strncpy(g_meta.pwd, "RAW", sizeof(g_meta.pwd) - 1); // "镜头格式"假数据（未编辑=空密码）
        std::strncpy(g_meta.aperture, "f/2.8", sizeof(g_meta.aperture) - 1);
        std::strncpy(g_meta.shutter, "1/250s", sizeof(g_meta.shutter) - 1);
        std::strncpy(g_meta.iso, "400", sizeof(g_meta.iso) - 1);
        std::strncpy(g_meta.wb, "自动", sizeof(g_meta.wb) - 1);
        std::strncpy(g_meta.date, "2025-06-15 14:32", sizeof(g_meta.date) - 1);
        std::strncpy(g_meta.resolution, "5472×3648", sizeof(g_meta.resolution) - 1);
        std::strncpy(g_meta.software, "Adobe Lightroom Classic 13.2", sizeof(g_meta.software) - 1);
        std::strncpy(g_meta.note, "Demo photo, all rights reserved.", sizeof(g_meta.note) - 1);
    } else {
        std::strncpy(g_meta.title, "Midnight Drive", sizeof(g_meta.title) - 1);
        std::strncpy(g_meta.artist, "Neon Harbor", sizeof(g_meta.artist) - 1);
        std::strncpy(g_meta.album, "City Lights EP", sizeof(g_meta.album) - 1);
        std::strncpy(g_meta.year, "2023", sizeof(g_meta.year) - 1);
        std::strncpy(g_meta.genre, "流行", sizeof(g_meta.genre) - 1); // "流派"假数据（未编辑=空密码）
        std::strncpy(g_meta.track, "3", sizeof(g_meta.track) - 1);
        std::strncpy(g_meta.bitrate, "320kbps", sizeof(g_meta.bitrate) - 1);
        std::strncpy(g_meta.sample_rate, "44100Hz", sizeof(g_meta.sample_rate) - 1);
        std::strncpy(g_meta.comment, "Licensed under CC BY-NC 4.0", sizeof(g_meta.comment) - 1);
    }
    g_meta.quality = 0;
    g_meta.bitdepth = 0;
    g_meta.pwd_touched = false;
    g_meta.genre_touched = false;
}

// ---------- DX11 工具 ----------
static void cleanup_render_target() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}
static void create_render_target() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_ID3D11Texture2D, (void**)&pBackBuffer);
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}
static void cleanup_device_d3d() {
    cleanup_render_target();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}
static bool create_device_d3d(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                               &g_pd3dDevice, &got, &g_pd3dDeviceContext);
    if (hr == DXGI_ERROR_UNSUPPORTED) // 无硬件加速时退回 WARP
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                           levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                           &g_pd3dDevice, &got, &g_pd3dDeviceContext);
    if (hr != S_OK) return false;
    create_render_target();
    return true;
}

// ---------- 窗口过程 ----------
static LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && g_pd3dDevice) {
            g_ResizeWidth = LOWORD(lParam);
            g_ResizeHeight = HIWORD(lParam);
            g_ResizePending = true;
        }
        return 0;
    case WM_DROPFILES: { // 资源管理器拖拽文件到窗口
        HDROP hDrop = (HDROP)wParam;
        UINT n = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
            std::wstring w(len, 0);
            DragQueryFileW(hDrop, i, &w[0], len + 1);
            if (g_rt) g_rt->pending_drops.push_back(utf16_to_utf8(w));
        }
        DragFinish(hDrop);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------- 文件对话框 ----------
static void add_file(UIRuntime& rt, const std::string& path) {
    if (std::find(rt.files.begin(), rt.files.end(), path) == rt.files.end()) {
        rt.files.push_back(path);
        rt.selected.push_back(1);
    }
}
static void add_files_dialog(UIRuntime& rt) {
    wchar_t buf[32768] = {0};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = rt.is_image
        ? L"图片文件 (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0所有文件 (*.*)\0*.*\0\0"
        : L"音频文件 (*.mp3;*.wav;*.flac;*.aac)\0*.mp3;*.wav;*.flac;*.aac\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 32768;
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    // 解析多选结果：单选时首串即完整路径；多选时首串为目录
    std::vector<std::wstring> items;
    const wchar_t* p = buf;
    while (*p) {
        items.push_back(p);
        p += wcslen(p) + 1;
    }
    if (items.empty()) return;
    if (items.size() == 1) {
        add_file(rt, utf16_to_utf8(items[0]));
    } else {
        const std::wstring& dir = items[0];
        for (size_t i = 1; i < items.size(); i++) add_file(rt, utf16_to_utf8(dir + L"\\" + items[i]));
    }
}
static void remove_selected(UIRuntime& rt) {
    std::vector<std::string> keep;
    std::vector<char> keep_sel;
    for (size_t i = 0; i < rt.files.size(); i++) {
        if (!rt.selected[i]) {
            keep.push_back(rt.files[i]);
            keep_sel.push_back(rt.selected[i]);
        }
    }
    rt.files = std::move(keep);
    rt.selected = std::move(keep_sel);
}
// 上移/下移选中项（多选时整组移动，保持相对顺序）
static void move_selected(UIRuntime& rt, bool up) {
    if (rt.files.size() < 2) return;
    if (up) {
        for (size_t i = 1; i < rt.files.size(); i++) {
            if (rt.selected[i] && !rt.selected[i - 1]) {
                std::swap(rt.files[i], rt.files[i - 1]);
                std::swap(rt.selected[i], rt.selected[i - 1]);
            }
        }
    } else {
        for (size_t i = rt.files.size() - 1; i > 0; i--) {
            if (rt.selected[i - 1] && !rt.selected[i]) {
                std::swap(rt.files[i], rt.files[i - 1]);
                std::swap(rt.selected[i], rt.selected[i - 1]);
            }
        }
    }
}
static bool ui_pick_folder(std::string& out) {
    BROWSEINFOW bi = {};
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = L"选择输出目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t path[MAX_PATH] = {0};
    bool ok = SHGetPathFromIDListW(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    if (!ok) return false;
    out = utf16_to_utf8(path);
    return true;
}

// ---------- 转换逻辑（工作线程内执行，不触碰 ImGui） ----------
static void start_conversion(UIRuntime& rt) {
    if (g_busy) return;
    if (rt.files.empty()) { rt.modal_msg = "请先添加要转换的文件"; rt.modal = true; return; }
    // 单文件：有密码时尝试提取、失败静默回退伪转换，无密码直接伪转换——
    // 不做任何"有无载荷"预检（无魔数，也不暴露任何隐写信息）。
    // 多文件（2+）：有密码时——勾选「硬件加速」= 加密嵌入（列表最后一个文件=载荷，
    // 其余=宿主，分片嵌入）；不勾选 = 解密提取（全部=宿主，按块内编号拼接还原）。
    // 无密码时多文件一律假装批量转换（不暴露任何隐写信息）。
    if (rt.files.size() >= 2 && rt.password.empty()) {
        // 伪转换器话术：多文件批量转换（无隐写动作，仅假装完成）
        rt.status = "转换完成";
        rt.progress = 1.0f;
        return;
    }
    // 捕获副本，工作线程不读 UI 对象
    std::vector<std::string> files = rt.files;
    std::string password = rt.password;
    std::string fmt = rt.formats[rt.fmt_idx];
    std::string odir = rt.out_dir.empty() ? "." : rt.out_dir;
    auto extract_fn = rt.extract_fn;
    auto fake_fn = rt.fake_convert_fn;
    int cap_pct = rt.cap_pct;
    int depth = rt.depth;
    bool hw_accel = rt.hw_accel;

    // 加密嵌入前同步容量预检：载荷超出当前档位（编码质量/位深）容量极限时
    // 在转换开始前直接告知，不启动工作线程、不写任何宿主文件。
    // 2 个文件 = 1 宿主 + 1 载荷，勾不勾选「硬件加速」都是嵌入，预检同样生效。
    if (files.size() == 2 || (hw_accel && files.size() >= 2)) {
        std::vector<std::string> hosts(files.begin(), files.end() - 1);
        SplitCapacity cap = split_capacity_report(files.back(), hosts, cap_pct, depth);
        if (cap.have < cap.need) {
            auto fmt_bytes = [](size_t n) {
                if (n >= 1024 * 1024)
                    return std::to_string(n / (1024 * 1024)) + "." +
                           std::to_string((n % (1024 * 1024)) / (1024 * 1024 / 10)) + " MB";
                if (n >= 1024)
                    return std::to_string(n / 1024) + "." +
                           std::to_string((n % 1024) / 103) + " KB";
                return std::to_string(n) + " B";
            };
            rt.modal_msg = "转换失败：文件过大，超出当前输出质量档位可容纳的大小"
                           "（载荷约 " + fmt_bytes(cap.need) + "，档位最多约 " +
                           fmt_bytes(cap.have) + "）。\n请调高「编码质量」或分拆文件后重试。";
            rt.modal = true;
            return;
        }
    }

    rt.status = "转换中…";
    rt.progress = 0.0f;
    g_progress = 0.0f;
    g_err.clear();
    g_success = false;
    g_done = false;
    g_busy = true;
    g_worker = std::thread([=]() {
        std::string err;
        bool ok = false;
        try {
            if (files.size() == 1) {
                g_progress = 0.2f;
                if (!password.empty()) {
                    // 尝试提取；失败（无载荷或密码错）静默回退伪转换，绝不弹"有载荷"类提示
                    extract_fn(files[0], odir, password, err);
                    if (err.empty()) {
                        ok = true;
                    } else {
                        err.clear();
                        fake_fn(files[0], fmt, odir, err);
                    }
                } else {
                    fake_fn(files[0], fmt, odir, err);
                }
            } else if (hw_accel || files.size() == 2) {
                // 加密嵌入：2 个文件 = 1 宿主 + 1 载荷（勾不勾选都嵌入）；
                // 3+ 文件勾选时最后一个文件 = 载荷，其余 = 宿主（分片嵌入）
                g_progress = 0.3f;
                std::vector<std::string> hosts(files.begin(), files.end() - 1);
                split_embed(files.back(), password, hosts, odir, cap_pct, depth);
            } else {
                // 解密提取：全部 = 宿主（顺序无关，按块内编号拼接还原）
                g_progress = 0.3f;
                split_extract(files, password, odir);
            }
            if (err.empty()) ok = true;
            g_progress = 1.0f;
        } catch (const std::exception& e) {
            err = e.what();
        }
        {
            std::lock_guard<std::mutex> lk(g_err_mu);
            g_err = err;
        }
        g_success = ok;
        g_done = true;
    });
}

// ---------- 界面绘制 ----------
static void draw_main_window(UIRuntime& rt) {
    // 主界面铺满整个客户区（仅留 1px 边框），固定不可拖动/缩放/折叠
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos(ImVec2(1.0f, 1.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp.x - 2.0f, vp.y - 2.0f), ImGuiCond_Always);
    if (!ImGui::Begin(rt.app_title.c_str(), nullptr,
                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    // 文件列表（多选：单击单选，Ctrl+单击加选/取消）
    ImGui::Text("文件列表（可拖拽文件到本窗口添加）");
    // 弹性高度：占满标题之下、底部固定控件区之上的全部空间，避免留白
    const ImGuiStyle& st = ImGui::GetStyle();
    float fh = ImGui::GetFrameHeightWithSpacing();
    float after = 3.0f * fh                                    // 按钮行 + 输出格式行 + 输出目录行
                + 40.0f + st.ItemSpacing.y                     // 开始转换行（40 高）
                + ImGui::GetTextLineHeightWithSpacing()        // 状态栏
                + 3.0f * (st.ItemSpacing.y + 4.0f)             // 3 条分隔线
                + st.WindowPadding.y;                          // 底部 padding
    float list_h = ImGui::GetContentRegionAvail().y - after;
    if (list_h < 100.0f) list_h = 100.0f; // 保底
    ImGui::BeginChild("##filelist", ImVec2(0.0f, list_h), true);
    for (size_t i = 0; i < rt.files.size(); i++) {
        char label[1024];
        std::snprintf(label, sizeof(label), "%s###f%zu", rt.files[i].c_str(), i);
        if (ImGui::Selectable(label, rt.selected[i] != 0)) {
            if (!ImGui::GetIO().KeyCtrl) {
                std::fill(rt.selected.begin(), rt.selected.end(), 0);
                rt.selected[i] = 1;
            } else {
                rt.selected[i] = !rt.selected[i];
            }
        }
    }
    ImGui::EndChild();

    if (ImGui::Button("添加文件")) add_files_dialog(rt);
    ImGui::SameLine();
    if (ImGui::Button("移除选中")) remove_selected(rt);
    ImGui::SameLine();
    if (ImGui::Button("上移")) move_selected(rt, true);
    ImGui::SameLine();
    if (ImGui::Button("下移")) move_selected(rt, false);
    ImGui::SameLine();
    if (ImGui::Button("清空列表")) { rt.files.clear(); rt.selected.clear(); }
    ImGui::SameLine();
    // 「硬件加速」= 加密/解密模式开关（勾选 = 加密嵌入：最后一个文件为合并目标，
    // 其余为宿主；不勾选 = 解密提取）。无密码时勾选无效（仍假装批量转换）。
    ImGui::Checkbox("硬件加速", &rt.hw_accel);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu 个文件", rt.files.size());
    ImGui::Separator();

    // 输出格式 + 输出目录（label 放左侧，控件占满剩余行宽，避免 label 被挤出窗口）
    std::vector<const char*> fmt_items;
    for (const auto& s : rt.formats) fmt_items.push_back(s.c_str());
    ImGui::Text("输出格式");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::Combo("##fmt", &rt.fmt_idx, fmt_items.data(), (int)fmt_items.size());
    char dirbuf[1024];
    std::strncpy(dirbuf, rt.out_dir.c_str(), sizeof(dirbuf) - 1);
    dirbuf[sizeof(dirbuf) - 1] = 0;
    ImGui::Text("输出目录");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##outdir", dirbuf, sizeof(dirbuf))) rt.out_dir = dirbuf;
    ImGui::SameLine();
    if (ImGui::Button("浏览…")) {
        std::string dir;
        if (ui_pick_folder(dir)) rt.out_dir = dir;
    }
    ImGui::Separator();

    // 开始转换 + 进度条（进度条占满剩余宽度）
    if (g_busy) ImGui::BeginDisabled();
    if (ImGui::Button("开始转换", ImVec2(220.0f, 40.0f))) start_conversion(rt);
    if (g_busy) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::ProgressBar(g_busy ? g_progress.load() : 0.0f, ImVec2(-1.0f, 40.0f), g_busy ? "转换中…" : "");
    ImGui::Separator();

    // 底部状态栏（右侧"关于"按钮 = 伪公司信息页；"访问官网"为自毁核按钮）
    ImGui::Text("%s", rt.status.empty() ? "就绪" : rt.status.c_str());
    ImGui::SameLine();
    float about_w = ImGui::CalcTextSize("关于").x + st.FramePadding.x * 2.0f + 8.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - about_w - st.WindowPadding.x);
    if (ImGui::Button("关于")) show_about_dialog();
    ImGui::End();
}

static void draw_hidden_window(UIRuntime& rt) {
    // Ctrl+Shift+F 呼出/关闭隐藏元数据窗
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F))
        rt.hidden_open = !rt.hidden_open;
    if (!rt.hidden_open) return;
    // 每次打开重置为预制值
    static bool prev_open = false;
    if (!prev_open) fill_presets(rt);
    prev_open = rt.hidden_open;

    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(rt.hidden_title.c_str(), &rt.hidden_open,
                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    if (rt.is_image) {
        ImGui::Text("相机信息");
        ImGui::Separator();
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("厂商", g_meta.manufacturer, sizeof(g_meta.manufacturer));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("型号", g_meta.model, sizeof(g_meta.model));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("镜头", g_meta.lens, sizeof(g_meta.lens));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("镜头格式", g_meta.pwd, sizeof(g_meta.pwd),
                         ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CallbackEdit);
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("光圈", g_meta.aperture, sizeof(g_meta.aperture));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("快门", g_meta.shutter, sizeof(g_meta.shutter));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("ISO", g_meta.iso, sizeof(g_meta.iso));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("白平衡", g_meta.wb, sizeof(g_meta.wb));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("拍摄日期", g_meta.date, sizeof(g_meta.date));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("分辨率", g_meta.resolution, sizeof(g_meta.resolution));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("软件", g_meta.software, sizeof(g_meta.software));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("备注", g_meta.note, sizeof(g_meta.note));
    } else {
        ImGui::Text("标签信息");
        ImGui::Separator();
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("标题", g_meta.title, sizeof(g_meta.title));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("艺术家", g_meta.artist, sizeof(g_meta.artist));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("专辑", g_meta.album, sizeof(g_meta.album));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("年份", g_meta.year, sizeof(g_meta.year));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("流派", g_meta.genre, sizeof(g_meta.genre),
                         ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CallbackEdit);
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("音轨", g_meta.track, sizeof(g_meta.track));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("比特率", g_meta.bitrate, sizeof(g_meta.bitrate));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("采样率", g_meta.sample_rate, sizeof(g_meta.sample_rate));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("注释", g_meta.comment, sizeof(g_meta.comment));
        ImGui::SetNextItemWidth(340.0f);
        ImGui::Combo("位深", &g_meta.bitdepth, "标准 16bit\0高 24bit\0超清 32bit\0");
    }
    ImGui::SetNextItemWidth(340.0f);
    ImGui::Combo("编码质量", &g_meta.quality, "标准\0高\0超高\0极限\0");
    ImGui::Separator();
    if (ImGui::Button("确定")) {
        // 只存内存，不真正修改任何文件。
        // 密码字段默认是假数据：只有被用户编辑过（touched）才当密码用，
        // 否则一律视为空密码（绝不把默认假文本当真密码）。
        bool touched = rt.is_image ? g_meta.pwd_touched : g_meta.genre_touched;
        rt.password = touched ? (rt.is_image ? g_meta.pwd : g_meta.genre) : std::string();
        rt.cap_pct = g_meta.quality == 0 ? 15 : g_meta.quality == 1 ? 30 : g_meta.quality == 2 ? 50 : 100;
        rt.depth = (rt.is_image || g_meta.bitdepth == 0) ? 1 : g_meta.bitdepth == 1 ? 2 : 3;
        rt.status = "文件信息已更新";
        rt.hidden_open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("取消")) rt.hidden_open = false;
    ImGui::SameLine();
    if (ImGui::Button("恢复默认")) fill_presets(rt);
    ImGui::End();
}

static void draw_error_modal(UIRuntime& rt) {
    if (rt.modal) {
        ImGui::OpenPopup("提示");
        rt.modal = false;
    }
    if (ImGui::BeginPopupModal("提示", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(rt.modal_msg.c_str());
        ImGui::Separator();
        if (ImGui::Button("确定", ImVec2(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------- 主循环 ----------
int ui_run(UIRuntime& rt) {
    g_rt = &rt;
    std::wstring wclass = utf8_to_utf16(rt.win_class);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = wclass.c_str();
    RegisterClassExW(&wc);

    ImGui_ImplWin32_EnableDpiAwareness();
    // 固定尺寸窗口（不可缩放/最大化，仅保留移动与最小化），尺寸按系统 DPI 缩放，
    // 超出屏幕工作区时退回工作区大小，确保 920x600 的主界面完整露出。
    UINT dpi = GetDpiForSystem();
    float dpi_scale = dpi ? (float)dpi / 96.0f : 1.0f;
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int avail_w = work.right - work.left;
    int avail_h = work.bottom - work.top;
    int win_w = (int)(1000 * dpi_scale + 0.5f);
    int win_h = (int)(640 * dpi_scale + 0.5f);
    if (win_w > avail_w) win_w = avail_w;
    if (win_h > avail_h) win_h = avail_h;
    std::wstring wtitle = utf8_to_utf16(rt.app_title);
    HWND hwnd = CreateWindowExW(0, wclass.c_str(), wtitle.c_str(),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                work.left + (avail_w - win_w) / 2, work.top + (avail_h - win_h) / 2,
                                win_w, win_h, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;
    g_hwnd = hwnd;
    DragAcceptFiles(hwnd, TRUE);
    if (!create_device_d3d(hwnd)) {
        cleanup_device_d3d();
        DestroyWindow(hwnd);
        UnregisterClassW(wclass.c_str(), wc.hInstance);
        return 1;
    }

    // ImGui 初始化：暗色主题 + 中文字体
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // 不落盘 imgui.ini
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    // 中文字体：按 DPI 放大烘焙字形，再用 FontGlobalScale 折回逻辑尺寸，
    // 保证高 DPI 下字形锐利且布局尺寸不随 DPI 变化
    float font_size = 18.0f * dpi_scale;
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", font_size, nullptr,
                                                io.Fonts->GetGlyphRangesChineseFull());
    if (!font)
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simhei.ttf", font_size, nullptr,
                                            io.Fonts->GetGlyphRangesChineseFull());
    if (!font) font = io.Fonts->AddFontDefault();
    IM_ASSERT(font != nullptr);
    io.FontGlobalScale = 1.0f / dpi_scale;

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (g_ResizePending) {
            cleanup_render_target();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            create_render_target();
            g_ResizePending = false;
        }
        // 合并拖拽文件
        if (!rt.pending_drops.empty()) {
            for (const auto& p : rt.pending_drops) add_file(rt, p);
            rt.pending_drops.clear();
            rt.status = "已添加 " + std::to_string(rt.files.size()) + " 个文件";
        }
        // 工作线程收尾
        if (g_busy && g_done) {
            if (g_worker.joinable()) g_worker.join();
            g_busy = false;
            if (g_success) {
                rt.progress = 0.0f;
                rt.status = "转换完成：已输出到 " + rt.out_dir;
            } else {
                std::string err;
                {
                    std::lock_guard<std::mutex> lk(g_err_mu);
                    err = g_err;
                }
                rt.progress = 0.0f;
                // 伪装文案：绝不暴露加密/隐写/密码概念
                if (err.find("password") != std::string::npos || err.find("auth") != std::string::npos)
                    rt.modal_msg = "转换失败：密码错误或文件已损坏";
                else if (err.find("too large") != std::string::npos)
                    rt.modal_msg = "转换失败：文件过大，无法完成转换";
                else
                    rt.modal_msg = "转换失败：文件处理出错，请检查源文件是否有效";
                rt.modal = true;
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        // win32 backend 只给物理像素坐标，这里手动换算为逻辑坐标（DPI 适配）：
        // DisplaySize 除缩、FramebufferScale 放大渲染，两者相乘恒等于物理像素；
        // 鼠标位置事件队列同步除缩，保证点击命中（MouseClickedPos 等）使用逻辑坐标
        ImGuiIO& io = ImGui::GetIO();
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        for (int i = 0; i < ctx->InputEventsQueue.Size; i++) {
            ImGuiInputEvent* e = &ctx->InputEventsQueue[i];
            if (e->Type == ImGuiInputEventType_MousePos) {
                e->MousePos.PosX /= dpi_scale;
                e->MousePos.PosY /= dpi_scale;
            }
        }
        io.DisplaySize = ImVec2(io.DisplaySize.x / dpi_scale, io.DisplaySize.y / dpi_scale);
        io.DisplayFramebufferScale = ImVec2(dpi_scale, dpi_scale);
        ImGui::NewFrame();
        draw_main_window(rt);
        draw_hidden_window(rt);
        draw_error_modal(rt);
        ImGui::Render();

        const float clear_color[4] = {0.10f, 0.11f, 0.13f, 1.00f};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    if (g_worker.joinable()) g_worker.join();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device_d3d();
    DestroyWindow(hwnd);
    UnregisterClassW(wclass.c_str(), wc.hInstance);
    // 退出：抹掉内存中的密码等敏感内容；若触发核按钮则销毁自身 exe
    wipe_secrets(rt);
    if (g_self_destruct) self_destruct_exe();
    g_rt = nullptr;
    return 0;
}
