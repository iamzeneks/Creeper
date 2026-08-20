#!/bin/sh
# package.sh — macOS 发行打包
# 用法：sh package.sh [VER]
# 依赖：clang++（C++17）、tar、gzip；dmg 打包可选（需 hdiutil，macOS 自带）
# 产物：
#   Creeper-<VER>-src-macos.tar.gz   源码包（可移植 C++17，无第三方库依赖）
#   Creeper-<VER>-macos.tar.gz       二进制包（creeper_cli + 文档 + README）
#   Creeper-<VER>-macos.dmg          （可选，见文末）拖拽式磁盘映像
set -e
cd "$(dirname "$0")"

VER=${1:-1.0}
ROOT=$(pwd)

sh build.sh   # 产出 build/creeper_cli（clang++ 自动选中）

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
tar -czf "Creeper-$VER-src-macos.tar.gz" -C build creeper-cli-src
rm -rf "$STAGE"
echo "packaged: Creeper-$VER-src-macos.tar.gz"

# ---------- 二进制包（CLI）----------
BIN="$ROOT/build/creeper-cli-bin"
rm -rf "$BIN"
mkdir -p "$BIN/docs"
cp build/creeper_cli "$BIN/"
cp docs/CLI_BUILD.md "$BIN/README.md"
cp docs/使用说明书.md docs/使用说明书.en.md docs/技术报告.md docs/技术报告.en.md "$BIN/docs/"
cp LICENSE THIRD_PARTY_LICENSES.md "$BIN/"
tar -czf "Creeper-$VER-macos.tar.gz" -C build creeper-cli-bin
rm -rf "$BIN"
echo "packaged: Creeper-$VER-macos.tar.gz"

# ---------- （可选）App Bundle + DMG ----------
# 说明：creeper_cli 是命令行工具，通常无需 .app；若日后增加 GUI 或用
# createInstaller 包装成可拖拽应用，取消下面注释即可。需 macOS 实机。
# APP="build/Creeper.app"
# rm -rf "$APP"
# mkdir -p "$APP/Contents/MacOS"
# cp build/creeper_cli "$APP/Contents/MacOS/"
# printf '<?xml version="1.0"?><plist><dict><key>CFBundleExecutable</key><string>creeper_cli</string><key>CFBundleIdentifier</key><string>com.creeper.cli</string></dict></plist>' > "$APP/Contents/Info.plist"
# hdiutil create -volname Creeper -srcfolder build/Creeper.app -ov -format UDZO "Creeper-$VER-macos.dmg"

echo "done. 发布前建议：codesign --sign 'Developer ID' build/creeper_cli 并公证（notarytool）"