# Creeper — 编码 Agent 任务书

[English](PROMPT_ENCODER.en.md) | **简体中文**

> 你是 **编码 agent**。任务：在项目根目录下实现一个 C++ 隐写工具（伪装成格式转换软件）。
> 所有规格**必须**严格按本文档执行，不得自行更改文件格式（测试 agent 依赖这些格式做交叉验证）。

## 0. 工作目录与现有资源（已就绪，勿动）

- 项目根：仓库根目录（`src/`、`tests/`、`docs/`、`res/` 所在目录）
- `stb/stb_image.h`、`stb/stb_image_write.h` — 已下载，**禁止修改**
- `imgui/`（完整 Dear ImGui 1.92+ master）+ `imgui/backends/`（imgui_impl_win32 + imgui_impl_dx11）— 已下载，**禁止修改**
- 宿主测试文件：`img.png`（8.8MB）、`msc.mp3`（20MB）、`src.png`（235MB，载荷素材）
- 编译器：`g++` (w64devkit 15.2.0)，可用链接库：`-lbcrypt -ld3d11 -ldxgi -lgdi32 -limm32 -luser32 -lshell32 -lole32 -luuid -lcomdlg32 -ldwmapi`
- 参考原型：`tests/envelope.py`（Python 版信封格式，**C++ 必须字节级兼容**，用于交叉验证）

## 1. 交付物清单

```
crypto.h / crypto.cpp      # BCrypt 加密信封 seal/open
png_steg.h / png_steg.cpp  # PNG LSB 隐写
mp3_steg.h / mp3_steg.cpp  # MP3 ID3v2 GEOB 隐写
common_ui.h / common_ui.cpp# 共享 GUI 基建（DX11 窗口、ImGui 初始化、中文字体、快捷键、文件对话框、UTF-8<->UTF-16 工具）
img_app.cpp                # 图片伪装转换器 → creeper_img.exe
audio_app.cpp              # 音频伪装转换器 → creeper_audio.exe
cli_main.cpp               # 无界面 CLI（测试用）→ creeper_cli.exe（源文件名避开 "creeper" 字样）
build.bat                  # 一键构建三个 exe
```
（说明书写小锌最后亲自负责，你不要写）

## 2. 加密信封格式（crypto.h/cpp）— 必须与 envelope.py 字节级兼容

```
偏移  大小  内容
0     8    magic  "CREEPER1"
8     1    version 0x01
9     16   salt（随机）
25    12   nonce（随机）
37    4    ct_len（大端）
41    ct_len  AES-256-GCM 密文（含 16B tag）
```

- KDF：**PBKDF2-HMAC-SHA256，600,000 次迭代**，派生 32 字节密钥
- 算法：**AES-256-GCM**（BCrypt：`BCRYPT_AES_ALGORITHM` + `BCRYPT_CHAINING_MODE_GCM`）
- 流程：seal = 读取文件字节 → PBKDF2 派生密钥 → AES-GCM 加密（随机 salt/nonce）→ 拼信封
- open = 解析信封 → PBKDF2 派生 → GCM 解密（认证失败 = 密码错或篡改，抛异常）
- **字符串混淆**：信封头 magic `"CREEPER1"` 与信封相关错误消息在源码中以 XOR 0x55 字节数组形式存在，运行时 `xstr()` 还原（每文件匿名命名空间内自备一份）；exe 内不得出现 `CREEPER1` 明文（含 `creeper` 字样的源文件名也不允许——CLI 主文件叫 `cli_main.cpp`）
- 接口：
  ```cpp
  std::vector<uint8_t> crypto_seal(const std::vector<uint8_t>& payload, const std::string& password);
  std::vector<uint8_t> crypto_open(const std::vector<uint8_t>& envelope, const std::string& password); // 失败抛 std::runtime_error
  ```
- BCrypt 要点：GCM 用 `BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO`（pbNonce=nonce，pbTag=16B）；PBKDF2 用 `BCryptDeriveKeyPBKDF2`（SHA256 算法句柄）；`BCryptSetProperty(BCRYPT_CHAINING_MODE, BCRYPT_CHAINING_MODE_GCM)`。

## 3. PNG 隐写（png_steg.h/cpp）

- 用 stb_image 解码（`stbi_load`，强制 RGB）；**只嵌入 RGB 三通道各 1 bit**（若有 alpha 保持原样）；用 stb_image_write 写回（`stbi_write_png`，无损）
- 位流顺序：像素行主序，每像素 R→G→B
- **嵌入方式 = ±1（LSB matching，非强制 LSB）**：目标位与像素当前 LSB 相同 → 不动；不同 → 像素值 ±1（方向随机 50/50；v=0 只可 +1、v=255 只可 -1）。避免强制 LSB 造成的相邻值频率相等（直方图阶跃）特征
- **直方图配对补偿**：嵌入完成后，统计修改后 RGB 汇总直方图 vs 原图直方图；遍历"未承载数据位"的像素（`used` 位图 → 像素级锁定），把"过剩 bin"（H'[v] > H[v]）的值 ±1 移向"亏缺 bin"（H[w'] < H[w]），每步更新 H'——把直方图拉回原形。提取端只读承载位 LSB，补偿移动不影响提取；承载像素（任一通道占用）绝不动
- **隐写头**（从第 0 位起顺序嵌入，**无魔数、无 seed 字段**——防固定特征扫描）：
  ```
  name_len 2B  大端（原始文件名长度）
  name     原始文件名 UTF-8（name_len 字节）
  env_len  4B  大端（信封长度）
  ```
- **散布种子由密码派生**（头+体整体散布）：`crypto_steg_seed(password, "creeper-seed")` = HMAC-SHA256(password, tag) 前 4 字节（轻量派生；seed 非机密，机密性由 GCM 保证）。**位置序列纯内容无关**（xorshift64 + 线性探测）——曾尝试"纹理掩码自适应嵌入"（4 邻域 diff ≥ 阈值才承载），但 ±1 修改影响邻居 diff 达 2，任何内容阈值都有模糊带导致掩码翻转、提取端序列无法重放，数学上不可行，已放弃
- **信封体**：按 seed 用 xorshift64 伪随机散布到位位置；`pos = xorshift() % total_bits`，若该位已被占用则线性探测 `(pos+1) % total_bits`（填充率低，探测极少；提取时同序重放，确定性一致）
- **自对抗重嵌**：嵌入 + 补偿后算 RGB 汇总直方图 L1，`L1 × 100 > 修改像素数 × 15` → 按 undo 快照恢复原图重嵌（最多 3 次；undo 不可设硬上限，截断导致恢复不全残留脏像素）；写盘前重放提取序列逐位自检（不一致抛 `png embed self-check failed: bit mismatch`）
- **载荷存在性 = GCM 认证**：无魔数 → 没有"一次性检测"；`png_has_payload(path, password)` 必须完整解析头 + 散布重放 + `crypto_open` 认证，密码错/无载荷/伪造数据一律返回 false（误报概率 2^-128）
- 容量检查（两道）：
  - **填充率上限**：`header_bits + env_bits > total_bits × fill_limit_pct / 100` → 抛 `payload too large: exceeds N% of host capacity (use --cap to raise the limit)`；`fill_limit_pct` 默认 15（抗统计检测，CLI `--cap` 可覆盖），`0` = 不限
  - **绝对容量**：`header_bits + env_bits > total_bits` → 抛 `payload too large for host image capacity`
- 接口：
  ```cpp
  bool png_has_payload(const std::string& path, const std::string& password); // 完整解析 + GCM 认证，失败一律 false
  void png_embed(const std::string& host_path, const std::string& payload_path,
                 const std::string& password, const std::string& out_path,
                 int fill_limit_pct = 15);
  void png_extract(const std::string& host_path, const std::string& password,
                   const std::string& out_dir); // 按头里原始文件名写出；密码错抛异常
  ```
- xorshift64 实现（确定性）：`uint64_t s = seed ? seed : 0x9E3779B97F4A7C15ULL; s ^= s<<13; s ^= s>>7; s ^= s<<17; return s;`（每次调用更新状态）

## 4. MP3 隐写（mp3_steg.h/cpp）— MPEG 帧头辅助位（音频数据零改动）

- **方案**：不碰标签、不碰音频数据，只改写每个 MPEG 音频帧头的 3 个辅助位（每帧 3 bit）：
  - `private`（第 2 字节 bit0，掩码 0x01）
  - `copyright`（第 3 字节 bit3，掩码 0x08）
  - `original`（第 3 字节 bit2，掩码 0x04）
  - 位序（一个 3 位值）：bit2=private、bit1=copyright、bit0=original；帧流顺序 = 位流顺序
- **帧扫描**（不依赖 ID3，音频起点后逐帧）：同步字 `0xFFE`；`version=(h1>>3)&3`（0=MPEG2.5, 2=MPEG2, 3=MPEG1，1=保留拒绝）、`layer=(h1>>1)&3`（须=1 Layer III）、`bri=(h2>>4)`（1..14）、`sri=(h2>>2)&3`（≤2）；帧长 = `nslot×bitrate×1000/samplerate + padding`，nslot=144（MPEG1）/72（MPEG2/2.5）；坏帧逐字节找同步。比特率/采样率表：kbps1={0,32,40,48,56,64,80,96,112,128,160,192,224,256,320}、kbps2={0,8,16,24,32,40,48,56,64,80,96,112,128,144,160}、sr1={44100,48000,32000}、sr2={22050,24000,16000}、sr25={11025,12000,8000}
- **嵌入**：把载荷 seal 成信封后构造位流（连续 bit 流，**无 magic**——防固定特征扫描）：
  ```
  name_len     16 bit（大端，UTF-8 文件名长度）
  name         name_len × 8 bit（UTF-8）
  env_len      32 bit（大端）
  env          env_len × 8 bit
  pad          填充 0 位到总位数是 3 的倍数（帧按 3 位消费，余位会截断）
  ```
- **帧头辅助位整体 XOR 密码派生 keystream**：`FrameKs(crypto_steg_seed(password, "creeper-ks"))` 每帧消费 3 位（帧序 = 扫描序），写入帧头的值 = 位流明文 ^ ks 3 位；提取端同样 XOR 还原明文。**只在帧层消费一次**（曾把 ks 放进 BitStream 组装+读取双重 XOR，导致两端阶段错位、自检失败）——密码错则解析出垃圾 → GCM 认证失败，与"无载荷"不可区分
- **容量检查**：帧数 × 3 ≥ 总位数，否则抛"payload too large"；嵌入 = 逐帧把位流 3 位/帧写入前 N 帧的辅助位（剩余帧保持原值）
- **检测**：无魔数 → `mp3_has_payload(path, password)` 收集位流 + 解析头 + `crypto_open` 认证，密码错/无载荷/伪造数据一律返回 false（误报概率 2^-128）
- **提取**：逐帧收集辅助位重建位流 → 解析 name_len/name/env_len/env → crypto_open → 按 name 写出到 out_dir
- 音频数据区（每帧头 4 字节之后）**逐字节零改动**；解码结果与宿主完全相同；标签区（ID3 前的字节）零改动
- 接口（与 png 同名同构）：
  ```cpp
  bool mp3_has_payload(const std::string& path, const std::string& password);
  void mp3_embed(const std::string& host_path, const std::string& payload_path,
                 const std::string& password, const std::string& out_path);
  void mp3_extract(const std::string& host_path, const std::string& password,
                   const std::string& out_dir);
  ```

## 4.5 WAV 隐写（wav_steg.h/cpp）— PCM 无损 LSB（大容量载体）

- **方案**：不碰 RIFF/fmt 区，只改 data 区每个样本最低 **depth 位**（depth 默认 1，可 2/3）：
  - 支持 PCM（format=1）8-bit 无符号 / 16-bit 有符号小端，任意声道数；样本顺序 = data 区顺序（多通道交错）
  - depth=1：嵌入 = **±1（LSB matching，非强制 LSB）**：目标位与样本 LSB 相同 → 不动；不同 → 样本 ±1（方向随机 50/50；8-bit 边界 0/255、16-bit 边界 ±32768 单方向）
  - depth=2/3：重写低 2/3 bit（样本差 ≤3 / ≤7，仅 16-bit 宿主；8-bit 宿主仅 depth=1）
  - 其他格式（float/A-law/压缩）→ 报错 `unsupported wav format (need PCM)` / `unsupported wav bit depth (need 8 or 16)`
- **隐写头**（无魔数、无 seed 字段；与 PNG/MP3 同构，前多 1B 承载深度）：
  ```
  depth    1B   承载深度（1/2/3；v1.0 旧格式无此字段）
  name_len 2B   大端（原始文件名长度）
  name     原始文件名 UTF-8（name_len 字节）
  env_len  4B   大端（信封长度）
  ```
- **解析新格式优先（首字节 = 1/2 合法），失败回退 v1.0 旧格式（无 depth 字段，depth=1）**——旧文件新代码可解，新文件旧代码视为无载荷
- **散布种子由密码派生**：`crypto_steg_seed(password, "creeper-uaz")`（注意：XOR 数组还原值是 creeper-uaz，非 creeper-wav）；头+信封体整体按 seed 用 xorshift64 伪随机散布（纯内容无关序列，占用线性探测；提取端从嵌入后文件精确重放）
- **容量检查**（两道，与 PNG 同）：填充率上限（`fill_limit_pct` 默认 15，CLI `--cap` 可覆盖，`0`=不限）→ `payload too large: exceeds N% of host capacity (use --cap to raise the limit)`；绝对容量（样本数×depth < 位数）→ `payload too large for host wav capacity`
- **自检**：写盘前重放提取序列逐位验证（不一致抛 `wav embed self-check failed: bit mismatch`）
- **检测/提取**：同 PNG 语义（`wav_has_payload(path, password)` 完整解析 + GCM 认证，密码错/无载荷一律 false；误报概率 2^-128）；提取按 name 写出
- **容量参考**：44.1kHz/16bit/stereo 每样本 depth 位：depth=1 ≈ 661 KB/分钟（100%）、depth=2 ≈ 1.3 MB、depth=3 ≈ 1.98 MB；默认 15% ≈ 99 KB（depth=1）/ 198 KB（2）/ 297 KB（3）；对比 MP3 ≈ 7.2 KB/整首
- 接口（与 png 同名同构，含 fill_limit_pct + depth；另有流式接口供分片复用）：
  ```cpp
  bool wav_has_payload(const std::string& path, const std::string& password);
  void wav_embed(const std::string& host_path, const std::string& payload_path,
                 const std::string& password, const std::string& out_path,
                 int fill_limit_pct = 15, int depth = 1);
  void wav_extract(const std::string& host_path, const std::string& password,
                   const std::string& out_dir);
  size_t wav_capacity(const std::string& host_path, int depth);
  std::vector<uint8_t> wav_read_stream(const std::string& host_path, const std::string& password);
  void wav_embed_stream(const std::string& host_path, const std::vector<uint8_t>& stream,
                        const std::string& password, const std::string& out_path,
                        int fill_limit_pct = 15, int depth = 1);
  ```

## 5. CLI（cli_main.cpp，测试用，无 GUI）

```
creeper_cli seal <in> <out> <password>
creeper_cli open <env> <out> <password>
creeper_cli embed <host> <payload> <out> <password> [--cap N] [--depth N]
creeper_cli extract <host> <outdir> <password>
creeper_cli has <host> <password>  # 输出 1/0；密码错或无载荷均为 0
creeper_cli split <payload> <password> <outdir> <host...> [--cap N] [--depth N]
creeper_cli unsplit <outdir> <password> <host...>
```
- `--cap N`：PNG/WAV 填充率上限覆盖（N=0..100，默认 15；仅 PNG/WAV 生效，MP3 忽略）
- `--depth 1|2|3`：WAV 承载深度（仅 WAV 生效，非 WAV 宿主带 --depth 报错；8-bit 宿主仅 depth=1）
- `split`：大文件拆分到多宿主（先整体 crypto_seal 再切分密文，分片协议见下方「分片协议」）；输出 `宿主_已转换.ext` 到 outdir；`--cap`/`--depth` 与 embed 同义
- `unsplit`：多宿主合并还原（宿主顺序无关，按块内编号拼接）
- 参数用 UTF-8；内部转 UTF-16 走 Windows API；stdout 用 printf 即可
- 所有失败：stderr 输出错误信息，exit code 非 0
- embed/extract/has/split/unsplit 的宿主类型按扩展名分发：.png → png_steg、.wav → wav_steg、.mp3 → mp3_steg；其他扩展名 → `unsupported host file type (need .png, .wav or .mp3)`

### 分片协议（split_steg.h/cpp）

- **原理**：payload 先整体 `crypto_seal`（一个 AES-GCM 信封，含 DEFLATE 压缩），再切分密文分头嵌进多宿主。**单块无意义**——任何单块都过不了 GCM 认证（提取端视为"无载荷"），必须收齐所有块按序号拼接后整体认证还原（单宿主截获无意义）
- **每宿主隐写流** = 标准隐写头（name_len+name+env_len；WAV 前多 1B depth）+ env = `magic(4B) + index(2B BE) + count(2B BE) + chunk_len(4B BE) + chunk`
  - magic = `crypto_steg_seed(password, "creeper-split")` 大端（密码派生无明文特征；密码错则 magic 不匹配 → 按普通信封解析 → GCM 认证失败 → 与"无载荷"不可区分）
  - count ≤ 65535；缺失块/重复块/非分片宿主跳过/报错
- **容量分配**：每宿主可用 = `floor(容量×填充率) − 头开销`，从列表开头顺序填装（MP3 按 100%——辅助位方案无填充率概念）；装完即止（多余宿主不输出）
- **接口**：
  ```cpp
  struct SplitCapacity { size_t need; size_t have; };
  SplitCapacity split_capacity_report(const std::string& payload_path,
                                      const std::vector<std::string>& hosts, int cap_pct, int depth);
  void split_embed(const std::string& payload_path, const std::string& password,
                   const std::vector<std::string>& hosts, const std::string& out_dir,
                   int cap_pct, int depth);
  void split_extract(const std::vector<std::string>& hosts, const std::string& password,
                     const std::string& out_dir);
  ```
- **容量预检**：`crypto_payload_size(payload)`（= DEFLATE 压缩后 + 41B 信封头，无需派生密钥）与 `split_embed` 共用 `compute_avail` 公式；GUI 在转换开始前同步预检，`have < need` 时直接告知不写宿主

## 6. GUI（img_app.cpp / audio_app.cpp / common_ui）— 伪装成格式转换软件

**两个 exe 共用 common_ui**（DX11 窗口 ~960x640 + ImGui 暗色主题 + 中文）。中文字体：`AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 18f, NULL, GetGlyphRangesChineseFull())`，失败回退 `simhei.ttf`。文件选择用 `GetOpenFileNameW`/`SHBrowseForFolderW`（UTF-8↔UTF-16 转换封装在 common_ui）。

**主界面（格式工厂风格）**：
- 标题：`格式转换大师 v3.2`（img）/ `音频转换专家 v2.8`（audio）
- 文件列表（可多选/拖拽添加，显示路径列表）+「添加文件」「移除选中」「上移」「下移」「清空列表」（「上移」「下移」多选整组移动、保持相对顺序）
- 「硬件加速」勾选框（多文件模式分派开关，见下）
- 「输出格式」下拉（img: JPG/PNG；audio: MP3/WAV）+「输出目录」选择
- 「开始转换」大按钮 + 进度条
- 底部状态栏

**转换逻辑（核心，按文件数分派）**：
- 列表里 **1 个文件**：**有密码**（隐藏窗口已填）→ 尝试 extract 到输出目录（文件名为原始名），**失败（密码错/无载荷/文件损坏）→ 静默回退伪转换**（绝不弹"无载荷"类提示、绝不暴露隐写概念）：img 用 stb 真转（jpg↔png，`stbi_load`+`stbi_write_png`），audio 直接复制+改后缀（.mp3→.wav）；**无密码** → 直接伪转换。输出名 = `原名_转换.新ext`
- 列表里 **2 个文件（1 宿主 + 1 载荷）**：有密码 → **加密嵌入**（第一个=宿主、最后一个=载荷，**勾不勾选「硬件加速」都一样嵌入**）；无密码 → 假装批量转换
- 列表里 **3+ 文件**：有密码时由「硬件加速」勾选分派——**勾选 = 加密嵌入**（最后一个=载荷，其余=宿主，分片嵌入），**不勾选 = 解密提取**（全部=宿主，顺序无关按块编号拼接）；无密码 → 一律假装批量转换（不暴露，勾选无效）
- **嵌入前容量预检**：加密嵌入前同步调用 `split_capacity_report`（与 split_embed 同公式）——载荷超出当前档位（编码质量/位深）容量极限时，转换开始前直接弹窗「转换失败：文件过大，超出当前输出质量档位可容纳的大小（载荷约 X，档位最多约 Y）。请调高「编码质量」或分拆文件后重试。」，不写任何宿主文件
- 密码未设置就 extract → 弹窗「转换失败：请先编辑文件信息」（不暴露隐写概念）
- 密码错误 → 弹窗「转换失败：密码错误或文件已损坏」（普通失败文案，不暴露隐写概念）
- embed 报 `too large`（填充率超限）→ 弹窗「转换失败：文件过大，无法完成转换」（不暴露隐写概念）

**隐藏元数据编辑窗（快捷键 Ctrl+Shift+F 呼出，`ImGui::IsKeyChordPressed(ImGuiMod_Ctrl|ImGuiMod_Shift, ImGuiKey_F)`）**：
- 窗口标题伪装：img = `EXIF 信息`；audio = `ID3 标签`
- **img 字段**（每次打开重置为以下预制值）：厂商=`Canon`、型号=`Canon EOS R6 Mark II`、镜头=`RF24-70mm F2.8 L IS USM`、**镜头格式 = `RAW`（假数据）**、光圈=`f/2.8`、快门=`1/250s`、ISO=`400`、白平衡=`自动`、拍摄日期=`2025-06-15 14:32`、分辨率=`5472×3648`、软件=`Adobe Lightroom Classic 13.2`、备注=`Demo photo, all rights reserved.`
- **audio 字段**：标题=`Midnight Drive`、艺术家=`Neon Harbor`、专辑=`City Lights EP`、年份=`2023`、**流派 = `流行`（假数据）**、音轨=`3`、比特率=`320kbps`、采样率=`44100Hz`、注释=`Licensed under CC BY-NC 4.0`
- 按钮：`确定`（把密码存入内存全局变量）、`取消`、`恢复默认`（重新填充预制值）
- 密码字段（镜头格式/流派）用 `ImGui::InputText(..., ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CallbackEdit, meta_pwd_cb)`（显示为 ••••，`meta_pwd_cb` 置 `pwd_touched`/`genre_touched`）；**未编辑过（touched=false）时点确定一律视为空密码**——默认假数据绝不当真密码用；「恢复默认」同时重置 touched
- 底部下拉：`编码质量`（标准 15% / 高 30% / 超高 50% / 极限 100% → 嵌入填充率上限，等效 CLI `--cap`，MP3 忽略）；audio 另有 `位深`（标准 16bit / 高 24bit / 超清 32bit → WAV 承载深度 1/2/3，等效 CLI `--depth`，MP3 忽略；img 窗无此字段）
- 说明：确定按钮**不真正修改任何文件**——纯内存存密码，元数据编辑是障眼

**伪装的要点**：所有可见文案、报错信息都围绕"格式转换器"，绝不出现 加密/隐写/密码 等字眼（密码字段在元数据窗里叫"镜头格式"/"流派"）。隐藏窗口标题、字段名要像真的 EXIF/ID3 编辑器。

**关于页与自毁**：
- 底部状态栏右侧「关于」按钮 → **原生 Win32 模态对话框**（真实独立弹窗，非 ImGui 共享窗口）：窗口类 `XhAboutDlg`（400×254 逻辑 × DPI 缩放，`WS_EX_DLGMODALFRAME`，含系统菜单可关闭）；内容 = 程序名 + 伪公司（北京星辉数媒科技有限公司）+ Copyright (C) 2024-2026 Beijing Xinghui Digital Media Co., Ltd. + 免责一行；按钮「关闭」「访问官网」
- 关于窗口为**真模态**：`show_about_dialog()` 内 `EnableWindow(g_hwnd, FALSE)` + 自跑 `GetMessageW` 模态循环（`WM_QUIT` 重新 `PostQuitMessage` 投递避免丢失），结束后 `EnableWindow(g_hwnd, TRUE)`
- 「关闭」→ `DestroyWindow`（`WM_COMMAND` IDC_ABOUT_CLOSE）；右上角 X 走 `WM_CLOSE` 同样销毁
- 「访问官网 www.xinghui-multimedia.cn」按钮 = 自毁核按钮：先 `DestroyWindow` 再弹 `MessageBoxW` 确认框（MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2，完整 URL 在确认框文案里）——「是」→ `g_self_destruct=true` + `PostMessageW(hwnd, WM_CLOSE)`；「否」→ 关闭确认框，进程继续
- 退出路径：`ui_run` 结束后调用 `wipe_secrets(rt)`（`SecureZeroMemory` 清密码缓冲、密码字符串清零）→ 若 `g_self_destruct` 调用 `self_destruct_exe()`
- `self_destruct_exe`：运行中 exe 无法直接删除（映像句柄无 FILE_SHARE_DELETE，改名再删也会被拒）→ 写临时 bat（`%TEMP%\msimg32_upd.bat`，无害化文件名，XOR 0x55 混淆）：`ping 127.0.0.1 -n 3` 等进程退出后 `del` exe 和 bat 自身；**路径用 8.3 短路径**（`GetShortPathNameW`，纯 ASCII），避免 bat 按控制台代码页解析中文路径乱码；`ShellExecuteW` SW_HIDE 静默执行
- 窗口类名（`CreeperImgApp`/`CreeperAudioApp`）与所有含 creeper 字样的字符串一律 XOR 0x55 混淆；GUI exe 内不得出现 `creeper` 明文

## 7. build.bat（一键构建）

```bat
@echo off
cd /d %~dp0
set LIBS=-lbcrypt -ld3d11 -ldxgi -lgdi32 -limm32 -luser32 -lshell32 -lole32 -luuid -lcomdlg32
g++ -O2 -std=c++17 -DUNICODE -D_UNICODE -mwindows -I. -Iimgui -Iimgui\backends -Istb ^
  img_app.cpp common_ui.cpp crypto.cpp png_steg.cpp mp3_steg.cpp wav_steg.cpp split_steg.cpp ^
  imgui\imgui.cpp imgui\imgui_draw.cpp imgui\imgui_tables.cpp imgui\imgui_widgets.cpp imgui\imgui_demo.cpp ^
  imgui\backends\imgui_impl_win32.cpp imgui\backends\imgui_impl_dx11.cpp -o creeper_img.exe %LIBS%
rem audio_app 同样构建 → creeper_audio.exe
rem creeper_cli 不加 -mwindows，链接同上（源码加 cli_main.cpp）→ creeper_cli.exe
```
（把三个构建命令都写全；`-mwindows` 仅用于两个 GUI exe）

## 8. 编码完成后自测（必须通过再交付）

1. `creeper_cli seal src.png env.bin 测试密码` → `open env.bin out.png 测试密码` → `fc /b src.png out.png` 一致
2. 错误密码 open → 非 0 退出
3. `embed img.png src.png steg.png 密码` → `has steg.png 密码` 输出 1 → `extract steg.png outdir 密码` → 与原文件一致
4. `embed msc.mp3 src.png steg.mp3 密码` → `extract steg.mp3 outdir 密码` → 一致；`has` 正确
5. `split 大文件 密码 outdir img.png msc.mp3 test.wav` → `unsplit outdir 密码 <3 个输出文件乱序>` → 还原与原文件一致
6. GUI 两个 exe 能启动、显示中文界面（人工确认）
7. 编译零警告（尽力而为）
8. 抽查三个 exe 二进制：不得含 `CREEPER1`/`CRPR`/`CRP`/`creeper` 明文（GUI exe 全查；CLI 仅允许 usage 帮助文本出现 `creeper_cli`）

## 9. 硬性约束

- **禁止修改 stb/ 和 imgui/ 下任何文件**
- 禁止引入新依赖（不下载任何东西）；仅用系统库 + 现有 stb/imgui
- 中文源码用 UTF-8 保存；不要用 `std::cout` 输出中文到控制台（CLI 错误信息用英文）
- 代码风格：简洁、注释中文、头文件带函数说明
- 完成后把 `build.bat` 跑一遍，确保三个 exe 都能构建出来，报告结果与任何偏离规格之处
