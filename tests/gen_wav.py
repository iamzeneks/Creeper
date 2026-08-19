# -*- coding: utf-8 -*-
"""生成 WAV 隐写测试宿主 test.wav（44.1kHz/16bit/stereo 60s 合成音频）。

用法：python tests/gen_wav.py [输出路径]（默认项目根 test.wav）
合成内容：两声道不同频率正弦 + 轻微噪声，纯确定性（无随机种子依赖）。
"""
import math
import os
import struct
import sys

RATE = 44100
CH = 2
BITS = 16
SECS = 60


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "test.wav")
    out = os.path.abspath(out)
    n = RATE * SECS
    data = bytearray()
    # 分块写，避免一次性构造 10MB+ 大列表
    for start in range(0, n, RATE):  # 每秒一块
        for i in range(start, start + RATE):
            t = i / RATE
            l = 12000 * math.sin(2 * math.pi * 440.0 * t) + 2000 * math.sin(2 * math.pi * 880.0 * t)
            r = 12000 * math.sin(2 * math.pi * 523.25 * t) + 2000 * math.sin(2 * math.pi * 1046.5 * t)
            data += struct.pack("<hh", int(l), int(r))
    data_len = len(data)
    header = b"RIFF" + struct.pack("<I", 36 + data_len) + b"WAVE" \
        + b"fmt " + struct.pack("<IHHIIHH", 16, 1, CH, RATE, RATE * CH * BITS // 8, CH * BITS // 8, BITS) \
        + b"data" + struct.pack("<I", data_len)
    with open(out, "wb") as f:
        f.write(header)
        f.write(data)
    print("written %s (%d bytes)" % (out, len(header) + data_len))
    return 0


if __name__ == "__main__":
    sys.exit(main())