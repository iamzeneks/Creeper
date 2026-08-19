# Creeper — 加密隐写套件

[English](README.en.md) | **简体中文**

> 把文件 **AES-256-GCM 加密**后，无损嵌入 PNG / MP3 / WAV 载体，携带者看不出任何异常。GUI 伪装成"格式转换器"，安全性只依赖密码（Kerckhoffs 原则）。

## 特性

- **端到端加密**：AES-256-GCM + PBKDF2-HMAC-SHA256（600,000 次迭代）；GCM 认证通过 = 载荷存在，误报概率 2⁻¹²⁸
- **三载体，音频数据/像素零破坏**：
  - PNG：RGB 三通道 ±1（LSB matching）+ 直方图配对补偿 + 自对抗重嵌
  - WAV：PCM 样本低 1–3 bit 无损改写（深度可配置，容量 ×3）
  - MP3：MPEG 帧头 3 个辅助位，音频数据区与 ID3 区逐字节零改动
- **无魔数**：隐写头无固定特征字节，散布种子由密码派生；存在性判定 = GCM 认证，密码错与无载荷不可区分
- **反统计检测**：默认 15% 填充率上限（可 `--cap` 调整）
- **双界面**：CLI（`creeper_cli`）+ 伪装 GUI（`creeper_img` / `creeper_audio`，含隐藏功能入口）
- **自校验**：写盘前重放提取序列逐位自检，嵌入后回读验证

## 安全模型

| 项 | 设计 |
|---|---|
| 加密 | AES-256-GCM（软实现，无外部依赖） |
| 密钥派生 | PBKDF2-HMAC-SHA256，600,000 迭代 |
| 载荷存在性 | GCM tag 认证（密码错/无载荷 → 同样结果，不可区分） |
| 散布 | 密码派生的 xorshift64 序列 + 线性探测，纯内容无关 |
| 统计暴露 | 15% 默认填充上限 + 直方图补偿 / 低位均匀化 |
| 互操作性 | C++ 与 Python 参考实现字节级互解（`tests/envelope.py`） |

## 载体容量

| 载体 | 嵌入方式 | 100% 容量 | 默认 15% 上限 |
|---|---|---|---|
| PNG（2560×1600） | RGB 各 1 bit，±1 修改 | ≈ 1.46 MB | ≈ 230 KB |
| WAV（44.1kHz stereo 每分钟） | 低 1 bit | ≈ 661 KB | ≈ 99 KB |
| WAV `--depth 2` | 低 2 bit | ≈ 1.3 MB | ≈ 198 KB |
| WAV `--depth 3` | 低 3 bit | ≈ 1.98 MB | ≈ 297 KB |
| MP3（19,194 帧） | 帧头辅助位 3 bit/帧 | ≈ 7.2 KB | — |

## 构建

需要 [w64devkit](https://github.com/skeeto/w64devkit)（`g++` 在 PATH）：

```bat
src\build.bat
```

产出三个可执行文件到 `res/`（发布包，与说明书 md 副本一起）：`creeper_cli.exe`（控制台）、`creeper_img.exe` / `creeper_audio.exe`（GUI，`-mwindows`）。第三方依赖 `imgui/`、`stb/` 已随仓库置于 `third_party/`。

## 测试

```bat
tests\run_all.bat        :: 一键全量回归（9 套件，任一失败退出非 0）
python tests\test_crypto.py   :: 加密往返 + 健壮性
python tests\test_png.py      :: PNG 隐写往返
python tests\test_mp3.py      :: MP3 隐写往返
python tests\test_wav.py      :: WAV 隐写往返（含 v1.0 旧格式兼容）
python tests\test_cross.py    :: C++ ↔ Python 信封互解（关键）
```

测试依赖 Python 3 + Pillow + numpy + cryptography。二进制测试宿主（`assets/img.png` / `assets/msc.mp3` / `assets/test.wav`）由 `tests/gen_hosts.py` 确定性生成（缺失时各套件自动补）。

## 快速上手（CLI）

```bat
:: 加密
creeper_cli seal 机密文件.pdf 机密文件.env 你的密码

:: 解密
creeper_cli open 机密文件.env 输出目录 你的密码

:: 嵌入（藏进载体）
creeper_cli embed 载体.png 机密文件.env 输出.png 你的密码

:: 提取
creeper_cli extract 输出.png 输出目录 你的密码

:: 检测（1=存在，0=无）
creeper_cli has 输出.png 你的密码

:: 大文件拆分到多宿主（先整体加密再切分，单块不可解）
creeper_cli split 大文件.bin 你的密码 输出目录 载体1.png 载体2.wav 载体3.mp3

:: 多宿主合并还原（宿主顺序无关，按块内编号拼接）
creeper_cli unsplit 输出目录 你的密码 载体3.mp3 载体1.png 载体2.wav
```

常用选项：`--cap N`（填充率上限 0–100，默认 15，PNG/WAV）；`--depth 1|2|3`（WAV 承载深度，容量 ×3）。

## GUI（伪装模式）

`creeper_img.exe`（图片"格式转换"）与 `creeper_audio.exe`（音频"格式转换"）界面呈现为普通格式转换器；真实功能经隐藏入口使用（`Ctrl+Shift+F` 呼出隐藏窗）。单文件模式：有密码 → 尝试提取，失败静默回退伪转换；无密码 → 伪转换。多文件模式（2+）由主界面「硬件加速」勾选分派：**勾选 = 加密嵌入**（列表最后一个文件 = 载荷，其余 = 宿主，自动分片到多个宿主；可用「上移」「下移」把载荷挪到最后），**不勾选 = 解密提取**（全部 = 宿主，乱序自动拼接还原）；无密码时多文件一律假装批量转换（不暴露任何信息）。隐藏窗另提供「编码质量」（填充率上限）与「位深」（WAV 深度 1/2/3）调节。

## 目录结构

```
├─ src/               源码（C++17，仅系统库）
│  └─ build.bat       构建脚本（w64devkit g++，输出 exe 到 res/）
├─ third_party/       imgui / stb（开源第三方，勿修改）
├─ docs/              规格任务书、测试报告、产品文档（md 源）
├─ res/               发布包（gitignore：exe + 说明书 md 副本）
├─ assets/            测试宿主（gitignore；tests/gen_hosts.py 生成）
├─ tests/             测试套件（Python / PowerShell）
├─ .github/           GitHub Actions CI（构建 + 非 GUI 套件）
├─ README.md / README.en.md / LICENSE / AGENTS.md
```

## 文档

- [使用说明书](docs/使用说明书.md)（产品手册）
- [技术报告](docs/技术报告.md)（设计与反检测原理）
- [编码任务书](docs/PROMPT_ENCODER.md) / [测试任务书](docs/PROMPT_TESTER.md)（规格）
- [测试报告](docs/TEST_REPORT.md)（缺陷与验证记录）

## 协议

[BSD 3-Clause](LICENSE)。仅用于合法目的（隐私保护、数据备份、媒体元数据隐藏等）；请遵守所在司法辖区的法律。

Copyright © 2026, Creeper Project Authors. All rights reserved.