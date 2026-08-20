# Creeper CLI — 编译安装指南（Linux / macOS）

Creeper 的纯命令行版：把文件**加密后藏进 PNG / MP3 / WAV 载体**。本包为源码包，附带一键编译脚本，适用于 Linux 与 macOS。

> 想要现成二进制？Windows 用户可直接用 `Creeper-CLI-1.0-win64.zip`（`creeper_cli.exe`）。下面说明源码编译流程。

## 一、快速开始

**依赖**：仅需 `g++` 或 `clang++`（支持 C++17），无任何第三方库依赖（`third_party/stb` 已随包提供）。

```sh
# 1) 解包
tar -xzf Creeper-1.0-src-linux.tar.gz      # Linux
tar -xzf Creeper-1.0-src-macos.tar.gz      # macOS
cd creeper-cli

# 2) 编译（自动选择 g++ 或 clang++）
sh build.sh

# 3) 验证
./build/creeper_cli
```

编译产物只有一个可执行文件 `build/creeper_cli`，可复制到任意目录单独使用（无动态库依赖）。

## 二、藏一个文件 / 取一个文件

```sh
# 把 机密.pdf 加密后藏进 载体.png，得到 输出.png（载体支持 PNG/MP3/WAV，输出同格式）
./build/creeper_cli embed 载体.png 机密.pdf 输出.png 你的密码

# 从 输出.png 取出 机密.pdf
./build/creeper_cli extract 输出.png 输出目录 你的密码
```

## 三、命令一览

| 命令 | 作用 |
|---|---|
| `seal <in> <out> <密码>` | 加密信封（不嵌载体） |
| `open <env> <out> <密码>` | 解密信封 |
| `embed <载体> <载荷> <out> <密码> [--cap N] [--depth N]` | 藏入载体 |
| `extract <载体> <输出目录> <密码>` | 从载体取出 |
| `has <载体> <密码>` | 检测是否有载荷（输出 1/0） |
| `split <载荷> <密码> <输出目录> <宿主...> [--cap N] [--depth N]` | 大文件分片到多宿主 |
| `unsplit <输出目录> <密码> <宿主...>` | 多宿主合并还原（顺序无关） |

常用选项：

- `--cap N`：填充率上限（0–100，默认 15，仅 PNG/WAV 生效）
- `--depth 1\|2\|3`：WAV 承载深度（默认 1，2/3 时容量 ×2/×3，仅 16-bit 宿主）

## 四、进阶构建

- 指定编译器：`CXX=clang++ sh build.sh`
- 产物目录：默认 `build/`，可用 `mkdir -p build && g++ -O2 -std=c++17 -Ithird_party -o build/creeper_cli src/cli_main.cpp src/crypto.cpp src/png_steg.cpp src/mp3_steg.cpp src/wav_steg.cpp src/split_steg.cpp` 手动编译。

## 五、注意事项

- 源码为 UTF-8，注释为中文；CLI 输出与错误信息均为英文。
- 隐写格式与 Windows 版、Python 参考实现（`tests/envelope.py`）字节级兼容，跨平台产物可互解。
- 本包不含二进制测试宿主；如需跑测试，请从仓库获取 `assets/` 与 `tests/`（需 Python3 + Pillow + numpy + cryptography）。

## 许可

[BSD 3-Clause](LICENSE)。仅用于合法目的。第三方组件：stb_image / stb_image_write（MIT / Public Domain 双许可），声明见 `THIRD_PARTY_LICENSES.md`。

---

## Creeper CLI — Build & Install Guide (English)

The command-line edition of Creeper: **encrypt a file and hide it inside a PNG / MP3 / WAV carrier**. This is a source package with a one-line build script for Linux and macOS.

> Want a ready-made binary? Windows users can grab `Creeper-CLI-1.0-win64.zip` (`creeper_cli.exe`). The rest of this guide covers building from source.

### 1. Quick start

**Dependencies:** just `g++` or `clang++` (C++17). No third-party libraries (the bundled `third_party/stb` is included).

```sh
tar -xzf Creeper-1.0-src-linux.tar.gz      # or Creeper-1.0-src-macos.tar.gz
cd creeper-cli
sh build.sh                                # picks g++ or clang++ automatically
./build/creeper_cli
```

The build produces a single static binary `build/creeper_cli` — copy it anywhere and run it (no dynamic deps).

### 2. Hide a file / get it back

```sh
./build/creeper_cli embed carrier.png secret.pdf output.png your-password
./build/creeper_cli extract output.png outdir your-password
```

### 3. Command reference

| Command | Purpose |
|---|---|
| `seal <in> <out> <password>` | Encrypted envelope (no carrier) |
| `open <env> <out> <password>` | Decrypt envelope |
| `embed <carrier> <payload> <out> <password> [--cap N] [--depth N]` | Hide into a carrier |
| `extract <carrier> <outdir> <password>` | Extract from a carrier |
| `has <carrier> <password>` | Detect payload (prints 1/0) |
| `split <payload> <password> <outdir> <hosts...> [--cap N] [--depth N]` | Split a large file across hosts |
| `unsplit <outdir> <password> <hosts...>` | Merge hosts back (order independent) |

Options: `--cap N` (fill-rate cap, 0–100, default 15, PNG/WAV only); `--depth 1|2|3` (WAV bit depth, default 1, 2/3 gives ×2/×3 capacity on 16-bit hosts).

### 4. Advanced build

- Choose compiler: `CXX=clang++ sh build.sh`
- Manual compile: `g++ -O2 -std=c++17 -Ithird_party -o build/creeper_cli src/cli_main.cpp src/crypto.cpp src/png_steg.cpp src/mp3_steg.cpp src/wav_steg.cpp src/split_steg.cpp`

### 5. Notes

- Sources are UTF-8 with Chinese comments; CLI output and errors are English.
- Stego format is byte-compatible across Windows builds and the Python reference implementation (`tests/envelope.py`) — cross-platform artifacts interoperate.
- This package has no binary test hosts; fetch `assets/` and `tests/` from the repository to run the test suite (needs Python3 + Pillow + numpy + cryptography).

### License

[BSD 3-Clause](LICENSE). For lawful purposes only. Third-party: stb_image / stb_image_write (MIT / Public Domain dual-licensed); see `THIRD_PARTY_LICENSES.md`.