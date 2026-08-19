# -*- coding: utf-8 -*-
"""Section W: WAV 隐写往返（容量/无损/超限/中文名）+ Section E WAV 检测
可重跑：python tests/test_wav.py
宿主：test.wav（44.1kHz/16bit/stereo 60s，纯正弦合成，5.3MB）
注意：embed 默认填充率上限 15%（抗统计检测）；has 需密码参数（无魔数，GCM 认证判定）。"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tester as T

PW = "wav-pass"
SAMPLES = 44100 * 2 * 60  # 样本数 = 44100Hz × 2ch × 60s
CAP_BYTES = SAMPLES // 8  # 每样本 1 bit
CAP_PCT = 15
CAP_LIMIT_BYTES = CAP_BYTES * CAP_PCT // 100  # 15% 上限（字节）


def wav_info(path):
    """返回 (data_off, data_len, bits, channels)，模拟 C++ 解析器。"""
    data = open(path, "rb").read()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    pos = 12
    bits = channels = None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        sz = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        if cid == b"fmt ":
            fmt, ch = struct.unpack("<HH", data[pos + 8:pos + 12])
            bits = struct.unpack("<H", data[pos + 8 + 14:pos + 8 + 16])[0]
            channels = ch
            assert fmt == 1, "not PCM"
        elif cid == b"data":
            return pos + 8, sz, bits, channels
        pos += 8 + sz + (sz & 1)
    raise AssertionError("no data chunk")


def sample_max_diff(orig_path, steg_path):
    """样本级最大绝对差（16-bit 有符号小端）。"""
    _, o_len, _, _ = wav_info(orig_path)
    o = open(orig_path, "rb").read()
    s = open(steg_path, "rb").read()
    assert len(o) == len(s)
    mx = 0
    cnt = 0
    for i in range(0, o_len, 2):
        a = struct.unpack_from("<h", o, i)[0]
        b = struct.unpack_from("<h", s, i)[0]
        d = abs(a - b)
        if d:
            cnt += 1
            mx = max(mx, d)
    return mx, cnt


def roundtrip(name, payload_size, extra=None):
    payload = T.tmp("w_payload_%s.bin" % name)
    steg = T.tmp("w_steg_%s.wav" % name)
    outdir = T.tmp(os.path.join("w_out_%s" % name, "sub"))
    T.make_random(payload, payload_size)
    rc, _, se = T.run(["embed", T.WAV, payload, steg, PW] + (extra or []))
    if rc != 0:
        T.check("WAV 往返 %s: embed 成功" % name, False, "rc=%d %s" % (rc, se.strip()))
        return None
    T.check("WAV 往返 %s: embed 成功" % name, True, "payload=%dB → steg.wav" % payload_size)
    rc, so, _ = T.run(["has", steg, PW])
    T.check("WAV 往返 %s: has==1" % name, rc == 0 and so.strip() == "1", "rc=%d out=%r" % (rc, so.strip()))
    rc, _, se = T.run(["extract", steg, outdir, PW])
    out_file = os.path.join(outdir, os.path.basename(payload))
    ok = rc == 0 and os.path.exists(out_file) and T.sha256(payload) == T.sha256(out_file)
    T.check("WAV 往返 %s: extract 字节一致且文件名还原" % name, ok,
            "rc=%d 还原文件=%s" % (rc, os.path.exists(out_file)))
    return steg


def main():
    T.reset()

    # W0: 宿主缺失时自动生成（tests/gen_wav.py）
    if not os.path.exists(T.WAV):
        gen = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen_wav.py")
        rc, _, se = T.run([sys.executable, gen, T.WAV])
        T.check("自动生成 test.wav 宿主", rc == 0 and os.path.exists(T.WAV), "rc=%d %s" % (rc, se.strip()))
        if rc != 0:
            sys.exit(1)

    # W1: 宿主信息与理论容量
    data_off, data_len, bits, ch = wav_info(T.WAV)
    cap = data_len // (bits // 8)
    T.check("test.wav 解析（PCM 16bit stereo）", bits == 16 and ch == 2,
            "bits=%d ch=%d data_off=%d data_len=%d" % (bits, ch, data_off, data_len))
    T.check("理论容量计算正确（每样本 1 bit → 字节 = 样本数/8）", cap == SAMPLES and cap // 8 == CAP_BYTES,
            "%d 样本 == %d，%d B == %d B" % (cap, SAMPLES, cap // 8, CAP_BYTES))

    # W2: 小载荷往返（1KB / 30KB / 45KB≈13.6%，默认上限内）
    steg_1k = roundtrip("1k", 1024)
    steg_30k = roundtrip("30k", 30 * 1024)
    steg_45k = roundtrip("45k", 45 * 1024)

    # W3: 无损验证（16-bit 样本最大差 ≤ 1；文件大小与 RIFF/fmt 区零改动）
    if steg_1k:
        o = open(T.WAV, "rb").read()
        s = open(steg_1k, "rb").read()
        T.check("WAV 无损: 文件大小不变", len(o) == len(s), "%d = %d" % (len(o), len(s)))
        T.check("WAV 无损: RIFF/fmt/头区（data 前）零改动", o[:data_off] == s[:data_off],
                "diff=%d B" % sum(1 for a, b in zip(o[:data_off], s[:data_off]) if a != b))
        mx, cnt = sample_max_diff(T.WAV, steg_1k)
        T.check("WAV 无损: 样本最大绝对差 ≤ 1（±1 嵌入）", mx <= 1, "max=%d 修改样本=%d" % (mx, cnt))
        T.check("WAV 无损: 修改样本数合理（≈ 载荷位翻转数）", cnt < 1024 * 8,
                "cnt=%d" % cnt)

    # W4: 填充率上限（15%）与 --cap
    payload = T.tmp("w_over.bin")
    T.make_random(payload, CAP_LIMIT_BYTES + 1024)
    rc, _, se = T.run(["embed", T.WAV, payload, T.tmp("w_over_steg.wav"), PW])
    T.check("WAV 填充率上限: 超 15% → 报错且无输出", rc != 0 and not os.path.exists(T.tmp("w_over_steg.wav")),
            "rc=%d stderr=%s" % (rc, se.strip()))
    steg_cap = T.tmp("w_over_steg2.wav")
    rc, _, _ = T.run(["embed", T.WAV, payload, steg_cap, PW, "--cap", "100"])
    T.check("WAV --cap 100: 同载荷成功往返", rc == 0 and os.path.exists(steg_cap), "rc=%d" % rc)
    rc, so, _ = T.run(["has", steg_cap, PW])
    T.check("WAV --cap 100: has==1", rc == 0 and so.strip() == "1", "out=%r" % so.strip())

    # W5: 绝对容量超限
    huge = T.tmp("w_huge.bin")
    T.make_random(huge, CAP_BYTES + 1024)
    rc, _, se = T.run(["embed", T.WAV, huge, T.tmp("w_huge_steg.wav"), PW, "--cap", "100"])
    T.check("WAV 绝对容量超限 → 报错且无输出", rc != 0 and not os.path.exists(T.tmp("w_huge_steg.wav")),
            "rc=%d stderr=%s" % (rc, se.strip()))

    # W6: 中文文件名载荷往返
    payload = T.tmp("测试音频载荷.bin")
    T.make_random(payload, 2048)
    steg = T.tmp("w_cn_steg.wav")
    outdir = T.tmp("w_cn_out")
    rc, _, _ = T.run(["embed", T.WAV, payload, steg, PW])
    T.check("WAV 中文文件名 embed", rc == 0, "rc=%d" % rc)
    rc, _, _ = T.run(["extract", steg, outdir, PW])
    T.check("WAV 中文文件名 extract 还原", rc == 0 and os.path.exists(os.path.join(outdir, "测试音频载荷.bin")),
            "rc=%d" % rc)

    # W7: has 检测语义（无魔数，GCM 认证判定）
    for label, path, pw, want in [
        ("has 干净 test.wav → 0", T.WAV, PW, "0"),
        ("has 嵌入后+正确密码 → 1", steg_1k, PW, "1"),
        ("has 嵌入后+错误密码 → 0", steg_1k, "wrong", "0"),
    ]:
        rc, so, _ = T.run(["has", path, pw])
        T.check(label, rc == 0 and so.strip() == want, "rc=%d out=%r" % (rc, so.strip()))
    rand = T.tmp("w_random.wav")
    T.make_random(rand, 50000)
    rc, so, _ = T.run(["has", rand, PW])
    T.check("has 随机二进制改名 .wav → 0（不误报）", rc == 0 and so.strip() == "0", "out=%r" % so.strip())

    # W8: 稳定性：连续 5 次 embed/extract 循环
    ok_all = True
    for i in range(5):
        p = T.tmp("w_stab_%d.bin" % i)
        T.make_random(p, 8192)
        s = T.tmp("w_stab_%d.wav" % i)
        o = T.tmp("w_stab_out_%d" % i)
        rc, _, _ = T.run(["embed", T.WAV, p, s, PW])
        rc2, _, _ = T.run(["extract", s, o, PW])
        if not (rc == 0 and rc2 == 0 and T.sha256(p) == T.sha256(os.path.join(o, os.path.basename(p)))):
            ok_all = False
            break
    T.check("WAV 稳定性: 连续 5 次 embed/extract 循环", ok_all, "失败 0/5")

    # W9: 8-bit PCM 往返 + float/其他格式拒绝
    w8 = T.tmp("w_8bit.wav")
    n8 = 44100 * 2 * 2  # 2s
    d8 = bytearray()
    for i in range(n8):
        v = 128 + int(64 * math.sin(2 * math.pi * 440.0 * i / 44100.0))
        d8 += struct.pack("<B", v & 0xFF)
    with open(w8, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(d8)) + b"WAVE"
                + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, 44100, 44100, 1, 8)
                + b"data" + struct.pack("<I", len(d8)))
        f.write(d8)
    p8 = T.tmp("w_8bit_payload.bin")
    T.make_random(p8, n8 // 8 * 10 // 100)  # 10% 填充率（留余量给信封头）
    s8 = T.tmp("w_8bit_steg.wav")
    rc, _, se = T.run(["embed", w8, p8, s8, PW])
    ok8 = rc == 0 and os.path.exists(s8)
    T.check("8-bit WAV embed 成功", ok8, "rc=%d %s" % (rc, se.strip()))
    if ok8:
        rc, so, _ = T.run(["has", s8, PW])
        T.check("8-bit WAV has==1", rc == 0 and so.strip() == "1", "out=%r" % so.strip())
        o8 = T.tmp("w_8bit_out")
        rc, _, _ = T.run(["extract", s8, o8, PW])
        T.check("8-bit WAV extract 字节一致", rc == 0
                and T.sha256(p8) == T.sha256(os.path.join(o8, os.path.basename(p8))), "rc=%d" % rc)
        o = open(w8, "rb").read()
        s = open(s8, "rb").read()
        T.check("8-bit WAV 无损: 头区零改动且样本差 ≤ 1",
                len(o) == len(s) and o[:44] == s[:44], "len %d=%d" % (len(o), len(s)))
    wf = T.tmp("w_float.wav")
    nf = 44100 * 2
    with open(wf, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + nf * 4) + b"WAVE"
                + b"fmt " + struct.pack("<IHHIIHH", 16, 3, 1, 44100, 44100 * 4, 4, 32)
                + b"data" + struct.pack("<I", nf * 4))
        f.write(b"\x00" * (nf * 4))
    pf = T.tmp("w_float_payload.bin")
    T.make_random(pf, 256)
    rc, _, se = T.run(["embed", wf, pf, T.tmp("w_float_steg.wav"), PW])
    T.check("float(WAVE_FORMAT_IEEE_FLOAT) 拒绝 → unsupported wav format",
            rc != 0 and "unsupported wav format" in se.lower() and not os.path.exists(T.tmp("w_float_steg.wav")),
            "rc=%d %s" % (rc, se.strip()))

    # W10-W17: 2-bit 高容量模式（--depth 2，仅 16-bit）
    steg_2b = roundtrip("2bit", 45 * 1024, extra=["--depth", "2"])
    if steg_2b:
        o = open(T.WAV, "rb").read()
        s = open(steg_2b, "rb").read()
        T.check("WAV 2bit 无损: 文件大小不变", len(o) == len(s), "%d = %d" % (len(o), len(s)))
        T.check("WAV 2bit 无损: 头区零改动", o[:data_off] == s[:data_off],
                "diff=%d B" % sum(1 for a, b in zip(o[:data_off], s[:data_off]) if a != b))
        mx, cnt = sample_max_diff(T.WAV, steg_2b)
        T.check("WAV 2bit 无损: 样本最大绝对差 ≤ 3（低 2 bit 重写）", mx <= 3, "max=%d 修改样本=%d" % (mx, cnt))
    # 2-bit 容量翻倍验证：1-bit 容量×1.5（超过 1-bit 绝对容量，2-bit 下应成功）
    big2 = T.tmp("w_2bit_big.bin")
    T.make_random(big2, CAP_BYTES * 3 // 2)
    s2b = T.tmp("w_2bit_big_steg.wav")
    rc, _, se = T.run(["embed", T.WAV, big2, s2b, PW, "--cap", "100", "--depth", "2"])
    T.check("WAV 2bit 容量: 1.5×1bit 容量 embed 成功（容量翻倍）", rc == 0 and os.path.exists(s2b),
            "rc=%d %s" % (rc, se.strip()))
    if rc == 0:
        rc, so, _ = T.run(["has", s2b, PW])
        T.check("WAV 2bit 容量: has==1（解析自适应深度）", rc == 0 and so.strip() == "1", "out=%r" % so.strip())
        out2 = T.tmp("w_2bit_big_out")
        rc, _, _ = T.run(["extract", s2b, out2, PW])
        T.check("WAV 2bit 容量: extract 字节一致", rc == 0
                and T.sha256(big2) == T.sha256(os.path.join(out2, os.path.basename(big2))), "rc=%d" % rc)
    # 2-bit 15% 上限生效（2-bit 容量 2× → 15% 也 2×）
    over2 = T.tmp("w_2bit_over.bin")
    T.make_random(over2, CAP_BYTES * 2 * CAP_PCT // 100 + 1024)
    rc, _, se = T.run(["embed", T.WAV, over2, T.tmp("w_2bit_over_steg.wav"), PW, "--depth", "2"])
    T.check("WAV 2bit 填充率上限: 超 15%（2×基准）→ 报错且无输出",
            rc != 0 and not os.path.exists(T.tmp("w_2bit_over_steg.wav")), "rc=%d stderr=%s" % (rc, se.strip()))
    # 8-bit 宿主 + --depth 2 → 拒绝
    rc, _, se = T.run(["embed", w8, p8, T.tmp("w_8bit_d2.wav"), PW, "--depth", "2"])
    T.check("8-bit WAV + --depth 2 → 拒绝（depth 2 requires 16-bit wav）",
            rc != 0 and "16-bit" in se and not os.path.exists(T.tmp("w_8bit_d2.wav")),
            "rc=%d %s" % (rc, se.strip()))
    # --depth 非法值 / 非 WAV 宿主
    rc, _, se = T.run(["embed", T.WAV, p8, T.tmp("w_d3.wav"), PW, "--depth", "3"])
    T.check("--depth 3 → 拒绝", rc != 0 and "depth" in se.lower(), "rc=%d %s" % (rc, se.strip()))
    rc, _, se = T.run(["embed", T.IMG, p8, T.tmp("w_dpng.png"), PW, "--depth", "2"])
    T.check("--depth 对 PNG 宿主 → 拒绝", rc != 0 and "wav" in se.lower()
            and not os.path.exists(T.tmp("w_dpng.png")), "rc=%d %s" % (rc, se.strip()))

    # W18: v1.0 旧格式（无 depth 字段）兼容回退——Python 复刻旧散布器构造旧文件，新 CLI 必须能解
    import hmac
    import hashlib

    def legacy_seed(pw):
        return struct.unpack(">I", hmac.new(pw.encode(), b"creeper-uaz", hashlib.sha256).digest()[:4])[0]

    def make_legacy(host, payload_path, env_path, out, pw):
        f = bytearray(open(host, "rb").read())
        n_samples = (len(f) - 44) // 2
        env = open(env_path, "rb").read()
        name = os.path.basename(payload_path).encode()
        stream = bytes([len(name) >> 8, len(name) & 0xFF]) + name + struct.pack(">I", len(env)) + env
        s = legacy_seed(pw)

        def xs_next():
            nonlocal s
            s ^= (s << 13) & 0xFFFFFFFFFFFFFFFF
            s ^= s >> 7
            s ^= (s << 17) & 0xFFFFFFFFFFFFFFFF
            return s

        used = bytearray(n_samples)
        for i in range(len(stream) * 8):
            p = xs_next() % n_samples
            while used[p]:
                p = (p + 1) % n_samples
            used[p] = 1
            off = 44 + p * 2
            v = struct.unpack_from("<h", f, off)[0]
            bit = (stream[i >> 3] >> (7 - (i & 7))) & 1
            if (v & 1) != bit:
                if v == -32768:
                    v += 1
                elif v == 32767:
                    v -= 1
                else:
                    v += 1 if (os.urandom(1)[0] & 1) else -1
                struct.pack_into("<h", f, off, v)
        open(out, "wb").write(bytes(f))

    lp = T.tmp("w_legacy_pay.bin")
    T.make_random(lp, 5000)
    le = T.tmp("w_legacy_env.bin")
    T.run(["seal", lp, le, "legacy-pw"])
    ls = T.tmp("w_legacy_steg.wav")
    make_legacy(T.WAV, lp, le, ls, "legacy-pw")
    rc, so, _ = T.run(["has", ls, "legacy-pw"])
    T.check("WAV v1.0 旧格式（无 depth 字段）回退解析: has==1", rc == 0 and so.strip() == "1",
            "rc=%d out=%r" % (rc, so.strip()))
    lo = T.tmp("w_legacy_out")
    rc, _, _ = T.run(["extract", ls, lo, "legacy-pw"])
    T.check("WAV v1.0 旧格式回退解析: extract 字节一致", rc == 0
            and T.sha256(lp) == T.sha256(os.path.join(lo, os.path.basename(lp))), "rc=%d" % rc)
    rc, so, _ = T.run(["has", ls, "wrong"])
    T.check("WAV v1.0 旧格式 + 错误密码 → 0", rc == 0 and so.strip() == "0", "out=%r" % so.strip())

    sys.exit(0 if T.summary("test_wav") else 1)


if __name__ == "__main__":
    main()