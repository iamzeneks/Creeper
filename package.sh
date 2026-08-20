#!/bin/sh
# package.sh — Linux 发行打包
# 用法：sh package.sh [VER]
# 依赖：g++ 或 clang++（C++17）、tar、gzip
# 产物：
#   Creeper-<VER>-src-linux.tar.gz   源码包（可移植 C++17，无第三方库依赖）
#   Creeper-<VER>-linux.tar.gz       二进制包（build/creeper_cli + 文档 + README）
set -e
cd "$(dirname "$0")"

VER=${1:-1.0}
ROOT=$(pwd)

sh build.sh   # 产出 build/creeper_cli

# ---------- 源码包 ----------
STAGE="$ROOT/build/creeper-cli-src"
rm -rf "$STAGE"
mkdir -p "$STAGE/src" "$STAGE/third_party/stb" "$STAGE/docs"
cp src/cli_main.cpp src/crypto.cpp src/crypto.h src/file_util.h \
   src/mp3_steg.cpp src/mp3_steg.h src/png_steg.cpp src/png_steg.h \
   src/split_steg.cpp src/split_steg.h src/wav_steg.cpp src/wav_steg.h \
   "$STAGE/src/"
cp third_party/stb/stb_image.h third_party/stb/stb_image_write.h "$STAGE/third_party/stb/"
cp docs/CLI_BUILD.md "$STAGE/README.md"
cp docs/使用说明书.md docs/使用说明书.en.md docs/技术报告.md docs/技术报告.en.md "$STAGE/docs/"
cp LICENSE THIRD_PARTY_LICENSES.md build.sh "$STAGE/"
tar -czf "Creeper-$VER-src-linux.tar.gz" -C build creeper-cli-src
rm -rf "$STAGE"
echo "packaged: Creeper-$VER-src-linux.tar.gz"

# ---------- 二进制包 ----------
BIN="$ROOT/build/creeper-cli-bin"
rm -rf "$BIN"
mkdir -p "$BIN/docs"
cp build/creeper_cli "$BIN/"
cp docs/CLI_BUILD.md "$BIN/README.md"
cp docs/使用说明书.md docs/使用说明书.en.md docs/技术报告.md docs/技术报告.en.md "$BIN/docs/"
cp LICENSE THIRD_PARTY_LICENSES.md "$BIN/"
tar -czf "Creeper-$VER-linux.tar.gz" -C build creeper-cli-bin
rm -rf "$BIN"
echo "packaged: Creeper-$VER-linux.tar.gz"

echo "done. (如需 deb/rpm，可用 fpm 基于二进制包二次封装)"