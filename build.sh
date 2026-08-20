#!/bin/sh
# build.sh — Linux / macOS 构建 Creeper CLI（源码包与仓库根目录通用）
# 用法：sh build.sh   （自动选 g++ 或 clang++，纯 C++17，仅依赖系统库 + third_party/stb）
# 产物：build/creeper_cli
set -e
cd "$(dirname "$0")"

if command -v g++ >/dev/null 2>&1; then
    CXX=g++
elif command -v clang++ >/dev/null 2>&1; then
    CXX=clang++
else
    echo "error: no g++ or clang++ found" >&2
    exit 1
fi

mkdir -p build
"$CXX" -O2 -std=c++17 -Ithird_party -o build/creeper_cli \
    src/cli_main.cpp src/crypto.cpp src/png_steg.cpp src/mp3_steg.cpp src/wav_steg.cpp src/split_steg.cpp

echo "built: build/creeper_cli"
echo "run:   ./build/creeper_cli   (see README for usage)"