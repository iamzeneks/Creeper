# PROMPT_TESTER.md — 算法测试 Agent 任务书

> 你是 **测试 agent**。任务：对 `C:\Users\Zeneks\Documents\code\creeper` 下已构建的 C++ 隐写工具做全面验证。
> 前置条件：编码 agent 已完成，`creeper_cli.exe`、`creeper_img.exe`、`creeper_audio.exe` 已构建于项目根目录。若 exe 不存在，先报告"构建缺失"，不要自己改代码。

## 0. 环境与资源

- 项目根：`C:\Users\Zeneks\Documents\code\creeper`
- CLI 用法（见 PROMPT_ENCODER.md §5）：
  `creeper_cli seal <in> <out> <password>` / `open <env> <out> <password>` / `embed <host> <payload> <out> <password> [--cap N] [--depth N]` / `extract <host> <outdir> <password>` / `has <host> <password>`（输出 1/0，密码错或无载荷均为 0） / `split <payload> <password> <outdir> <host...> [--cap N] [--depth N]` / `unsplit <outdir> <password> <host...>`
- 宿主：`img.png`（8.8MB）、`msc.mp3`（20MB）；载荷素材：`src.png`（235MB）
- Python 3.14.4 + Pillow 12.2.0 + cryptography 49.0.0 + numpy（可用）
- 参考信封实现（字节级兼容基准）：`C:\Users\Zeneks\.openclaw\workspace\creeper\envelope.py`（`seal(payload, password)` / `open_seal(envelope, password)`）

## 1. 测试矩阵（全部要跑，结果记录进 TEST_REPORT.md）

### A. 加密往返（CLI seal/open）
- 随机数据文件：1B、1KB、1MB、10MB（`os.urandom` 生成）；密码：普通 ASCII + 中文密码 + 空密码（空密码若 CLI 接受）
- 断言：`open` 输出与输入 `hashlib.sha256` 一致
- 负例：错误密码 → 非 0 退出码且无输出文件；篡改信封任意 1 bit → 非 0 退出

### B. 跨语言交叉验证（关键！验证 C++ 与 Python 信封字节级兼容）
- C++ `seal` 的输出 → Python `open_seal` 必须成功且还原原数据
- Python `seal` 的输出 → C++ `open` 必须成功且还原原数据
- 同一输入+同一密码，两边信封头（magic/version/salt/nonce 布局）长度一致（41 字节头 + 密文）
- 注意：salt/nonce 随机，两边密文不同是正常的；只验证**互解**，不验证密文相等

### C. PNG 隐写往返
- 用 PIL 读取 `img.png`，报告尺寸与理论容量（w×h×3÷8 字节）
- 构造小载荷（1KB、容量 15% 以内如 150KB/200KB）→ `embed img.png payload steg.png 密码` → `has steg.png 密码`==1 → `extract` → 字节一致
- **填充率上限（默认 15%）**：造 ~16% 容量载荷（如 250KB）→ embed 报 `payload too large: exceeds 15%...` 且非 0 退出、无输出文件；同载荷加 `--cap 100` → 成功往返。`--cap` 边界值 0/100 也测
- **宿主无损验证**：PIL 能打开 steg.png；与原图逐像素比较（numpy），单通道最大绝对差必须 ≤ 1（±1 嵌入），alpha 不变；平均绝对差只对小填充率载荷断言（高填充率下平均差≈0.5×填充率>1/255 属正常）
- **直方图抗检测验证**（±1 嵌入 + 配对补偿后的统计特征）：
  - 原图 vs steg.png 的 RGB 汇总直方图 L1 距离必须 ≤ 修改通道像素数 × 0.1（配对补偿后应远低于该阈值，典型比例 ~0.01）
  - 相邻 bin 相等数量（H'[v]==H'[v+1]，LSB 强制嵌入的典型痕迹）必须 ≤ 修改通道像素数 × 0.01
- **容量超限**：造一个超过绝对容量的载荷（如 1.6MB）→ embed 必须报错且非 0 退出；二分实测 15% 上限下的最大可嵌入字节数（预期 ≈ 容量 15% × 0.95 上下，因信封膨胀与头开销）
- 用 `src.png` 试一次 embed 进 `img.png`，记录结果（预期：超 15% 上限报错——如实记录，这是说明书素材）
- 中文文件名载荷：`测试载荷.bin` → extract 后文件名还原
- **无魔数验证**：嵌入后宿主文件中不得存在固定魔数特征；`has` 对 干净宿主→0 / 嵌入后+正确密码→1 / 嵌入后+错误密码→0 / 随机二进制改名 .png→0
- **密码化散布验证（NO-MAGIC-2）**：同一载荷同一宿主、不同密码 → 两次 embed 的 steg.png 像素差异位分布应完全不同（散布位置随密码变化，无固定位置）；隐写头无 seed 字段（位流总长 = 2 + name_len + 4 + env_len 字节，散布读不出 16 bit 长度的位置不再固定）

### D. MP3 隐写往返
- `embed msc.mp3 payload steg.mp3 密码`（载荷 ≤ 容量，~6KB）→ `has steg.mp3 密码`==1 → `extract` → 字节一致
- **宿主零侵入验证**（Python 手工解析 MPEG 帧）：
  - 文件大小完全不变
  - ID3v2 标签区（音频起点前）逐字节完全一致
  - 逐字节比较全文件：差异掩码只可能是 {0x01, 0x04, 0x08, 0x0C}（private/copyright/original 及其组合），**无其他任何掩码**
  - 帧数据区（每帧头 4 字节之后到下一帧）逐字节零改动
  - 前 8 帧辅助位拼成 24 位**不得等于**旧魔数 0x435250（无固定特征）；**且随密码变化**（帧头辅助位整体 XOR 密码派生 keystream，NO-MAGIC-2）——同一宿主同一载荷、不同密码，前 8 帧辅助位不同
- 原 msc.mp3 无 ID3v2 的情况也要测：构造一个无标签的 MP3（拷贝 msc.mp3 去掉 ID3 头）→ embed → extract 往返 → 帧数据区零改动
- 中文文件名载荷同样测
- **容量超限**：载荷超过 帧数×3 bit 容量 → embed 必须报错且非 0 退出、不产生输出文件
- 解码无损：steg.mp3 与 msc.mp3 用同一解码器输出完全相同（帧数据未动，语义上必然成立）

### E. 检测功能（has）
- 干净宿主（带密码）→ 0；嵌入后+正确密码 → 1；嵌入后+错误密码 → 0（无魔数，认证判定）；随机二进制改名 .png/.mp3/.wav → 0（不应误报）
- 缺密码参数（`has <host>`）→ 非 0 退出（usage）

### W. WAV 隐写往返（PCM 无损 LSB，大容量载体）
- 宿主 `test.wav`（44.1kHz/16bit/stereo 60s，理论容量 = 样本数/8 ≈ 661.5KB；15% 上限 ≈ 99KB）
- 小载荷（1KB / 30KB / 45KB≈13.6%）→ `embed test.wav payload steg.wav 密码` → `has steg.wav 密码`==1 → `extract` → 字节一致
- **宿主零侵入验证**（Python 解析 RIFF/fmt/data）：
  - 文件大小完全不变；data chunk 之前（RIFF/fmt 区）逐字节完全一致
  - 样本级最大绝对差 ≤ 1（±1 嵌入）；修改样本数 ≈ 载荷位翻转数
- **depth（承载深度）1/2/3**（`--depth 1|2|3`）：
  - depth=2 往返：样本差 ≤3、容量 ×2（15% 上限 2× 基准）；depth=3 往返：样本差 ≤7、容量 ×3
  - 8-bit 宿主 depth=2/3 → 报错（仅 16-bit 支持）；非 WAV 宿主带 `--depth` → 报错；`--depth 4` → 报错
  - v1.0 旧格式（无 depth 字段）回退：新代码可解旧文件（新格式解析失败自动回退）
- **填充率上限（默认 15%）**：超限载荷 → 报 `payload too large: exceeds 15%...` 且无输出；同载荷 `--cap 100` → 成功往返；绝对容量超限（> 样本数×depth）→ 报错
- 中文文件名载荷往返；随机二进制改名 .wav → has 0
- 稳定性：连续 5 次 embed/extract 循环
- 构造 8-bit WAV 宿主（可选）验证位深分支；float WAV（可选）验证 `unsupported wav format`

### S. 大文件分片多宿主（split/unsplit）
- **混合宿主往返**：`split` 一个超过单个宿主容量的载荷到多宿主（PNG+WAV、PNG+WAV+MP3 混合）→ `unsplit` 乱序还原 → 与原文件字节一致
- **顺序无关**：乱序给 `unsplit` → 还原一致
- **缺块**：少给一个块 → `unsplit` 报 `missing split blocks (need N, have M)` 非 0 退出
- **密码错** → `no payload found`；**单块 extract/has** → 无载荷（单块过不了 GCM 认证）；**非分片普通宿主**混入 → 跳过或 `no payload found`
- **容量不足**：载荷超出多宿主总和 → `split` 报 `payload too large for combined host capacity` 非 0 退出、不产出
- **depth=2 + --cap 30 分片**：WAV 高容量分片往返
- **空密码 split/unsplit** → 成功往返

### F. GUI 冒烟（PowerShell）
- 启动 `creeper_img.exe` → 等 3 秒 → `Get-Process creeper_img` 存在 → 杀进程；`creeper_audio.exe` 同理
- 记录窗口是否正常出现（无法截图就记录进程存活）
- 关于页/自毁：可自动化验证（PowerShell P/Invoke + `EnumWindows` 找类 `XhAboutDlg`，注意 FindWindowW 只能找 top-level、`GetClassNameW` 需 `CharSet=Unicode`）：弹窗打开 → 按钮 `BM_CLICK`（**用 `PostMessage` 异步，`SendMessage` 会阻塞在 MessageBox 模态**）→ `#32770` 确认框出现 → 点「否」进程存活 / 点「是」进程退出且 exe 被删（副本验证）；已按此真实验证全链路，见 TEST_REPORT §12
- **GUI 专项脚本**：`test_gui_about.ps1`（关于弹窗真实鼠标点击链路 + 副本自毁）；`test_gui_quality.ps1`（Ctrl+Shift+F 打开隐藏窗 → Windows OCR 验证「编码质量」下拉存在）；两个脚本均需 `SetWindowPos(HWND_TOPMOST)` + 窗口中心点击激活绕过前台锁定（否则被遮挡偶发失败）
- **二进制抽查**：GUI exe 内不得含 `CREEPER1`/`CRPR`/`CRP`/`creeper` 明文；CLI exe 仅允许 usage 帮助文本含 `creeper_cli`，不得含 `CREEPER1`/`CRPR`/`CRP` 及 `creeper_cli.cpp` 等源文件名

### G. 健壮性
- 缺参数、文件不存在、宿主是空文件、载荷为空文件 → 全部要干净报错（非 0 退出，不崩溃、不写垃圾输出）

## 2. 交付物（写入项目根）

- `tests/test_crypto.py`、`tests/test_png.py`、`tests/test_mp3.py`、`tests/test_wav.py`、`tests/test_split.py`、`tests/test_cross.py`（Python，可直接重跑）
- `tests/test_gui.ps1`、`tests/test_gui_about.ps1`、`tests/test_gui_quality.ps1`（PowerShell）
- `tests/run_all.bat`（一键串行全量回归，9 套件任一失败退出非 0）
- 测试基建：`tests/tester.py`（公共工具，ROOT 已相对化）、`tests/envelope.py`（信封参考实现本地副本，test_cross 优先本地）、`tests/gen_hosts.py` / `tests/gen_wav.py`（宿主自动生成）
- `TEST_REPORT.md`：**表格**记录每项用例 → 通过/失败/说明；失败项附复现命令与现象

## 3. 约束

- **禁止修改任何生产代码**（crypto/png_steg/mp3_steg/common_ui/*_app.cpp、stb、imgui、build.bat）；发现 bug 只记录复现步骤
- 临时测试文件放 `tests/tmp/`，测完清理大文件（保留报告）
- 用 `fc /b` 或 Python 字节比较均可，报告里注明方法
- 报告用中文
