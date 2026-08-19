# -*- coding: utf-8 -*-
"""Section D: MP3 帧头辅助位隐写（private/copyright/original，3bit/帧，音频数据零改动）
Section E: MP3 检测
可重跑：python tests/test_mp3.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tester as T

PW = "mp3-pass"
AUDIO_CMP = 1024 * 1024  # 音频数据区比较字节数

KBPS1 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
KBPS2 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160]
SR1 = [44100, 48000, 32000]
SR2 = [22050, 24000, 16000]
SR25 = [11025, 12000, 8000]


def audio_start(data):
    """音频区起点：ID3v2 标签后（跳过 padding 全零）"""
    off = 0
    if data[:3] == b"ID3" and len(data) >= 10:
        major, flags = data[3], data[5]
        if 2 <= major <= 4 and not (flags & 0x0F):
            sz = ((data[6] & 0x7F) << 21) | ((data[7] & 0x7F) << 14) | ((data[8] & 0x7F) << 7) | (data[9] & 0x7F)
            off = 10 + sz
            while off < len(data) and data[off] == 0:
                off += 1
    return off


def scan_frames(data):
    """返回帧头偏移列表（与 C++ mp3_steg.cpp scan_frames 一致）"""
    offs = []
    pos = audio_start(data)
    while pos + 4 <= len(data):
        h = data[pos:pos + 4]
        if h[0] == 0xFF and (h[1] & 0xE0) == 0xE0:
            ver = (h[1] >> 3) & 3
            layer = (h[1] >> 1) & 3
            bri = h[2] >> 4
            sri = (h[2] >> 2) & 3
            if ver != 1 and layer == 1 and 1 <= bri <= 14 and sri <= 2:
                if ver == 3:
                    br, srate, nslot = KBPS1[bri], SR1[sri], 144
                else:
                    br, srate, nslot = KBPS2[bri], SR2[sri], 72
                fsz = nslot * br * 1000 // srate + ((h[2] >> 1) & 1)
                if fsz >= 4 and pos + fsz <= len(data):
                    offs.append(pos)
                    pos += fsz
                    continue
        pos += 1
    return offs


def aux_bits(h):
    """帧头辅助 3 位：bit2=private, bit1=copyright, bit0=original"""
    return ((h[2] & 0x01) << 2) | (((h[3] >> 3) & 0x01) << 1) | ((h[3] >> 2) & 0x01)


def frame_data_diff(orig, st, offs):
    """帧数据区（帧头 4 字节之后到下一帧）逐字节 diff；返回 (帧头外 diff 数, 帧头内 diff 掩码集)"""
    n_extra = 0
    masks = set()
    for i, off in enumerate(offs):
        end = offs[i + 1] if i + 1 < len(offs) else min(len(orig), len(st))
        for p in range(off, end):
            if orig[p] != st[p]:
                if p - off < 4:
                    masks.add(orig[p] ^ st[p])
                else:
                    n_extra += 1
    return n_extra, masks


def roundtrip(payload, steg, outdir, host=T.MP3):
    """embed → has → extract → 字节一致；返回 steg 路径"""
    rc, _, se = T.run(["embed", host, payload, steg, PW])
    if rc != 0:
        T.check("MP3 往返 embed 成功", False, "rc=%d %s" % (rc, se.strip()))
        return None
    rc, so, _ = T.run(["has", steg, PW])
    T.check("MP3 往返 has==1", rc == 0 and so.strip() == "1", "rc=%d out=%r" % (rc, so.strip()))
    rc, _, se = T.run(["extract", steg, outdir, PW])
    out_file = os.path.join(outdir, os.path.basename(payload))
    ok = rc == 0 and os.path.exists(out_file) and T.sha256(payload) == T.sha256(out_file)
    T.check("MP3 往返 extract 字节一致且文件名还原", ok,
            "rc=%d 还原文件存在=%s err=%r" % (rc, os.path.exists(out_file), se.strip()[:200]))
    return steg


def main():
    T.reset()

    # D1: 标准往返（载荷接近容量上限：19194 帧 × 3bit ≈ 7197B）
    payload = T.tmp("d_mp3_6k.bin")
    T.make_random(payload, 6 * 1024)
    steg = T.tmp("d_steg.mp3")
    outdir = T.tmp("d_out")
    roundtrip(payload, steg, outdir)

    # D2: 宿主零侵入验证（帧头辅助位之外一律不变）
    if os.path.exists(steg):
        orig = open(T.MP3, "rb").read()
        st = open(steg, "rb").read()
        T.check("文件大小完全不变", len(orig) == len(st),
                "orig=%d steg=%d" % (len(orig), len(st)))
        offs = scan_frames(orig)
        T.check("MPEG 帧解析成功（帧数>1000）", len(offs) > 1000, "帧数=%d" % len(offs))
        # 标签区（音频起点前）逐字节一致
        ao = audio_start(orig)
        T.check("ID3v2 标签区逐字节完全一致", orig[:ao] == st[:ao],
                "标签区=%d 字节" % ao)
        # 帧数据区零改动 + 帧头只改辅助位
        n_extra, masks = frame_data_diff(orig, st, offs)
        allowed = {0x01, 0x04, 0x08, 0x0C}
        T.check("帧数据区（帧头之外）零改动", n_extra == 0, "帧头外差异=%d 字节" % n_extra)
        T.check("帧头差异仅辅助位（掩码⊆{1,4,8,C}）", masks <= allowed, "掩码=%s" % sorted(masks))
        # 无魔数：前 8 帧辅助位（24 bit）是 name_len 等头字段，不再是固定 "CRP" 特征
        magic = 0
        for i in range(8):
            magic = (magic << 3) | aux_bits(st[offs[i]:offs[i] + 4])
        T.check("steg.mp3 无固定魔数（前 24 位 ≠ 旧 CRP 特征）", magic != 0x435250,
                "前24位=%06x" % magic)

    # D3: 无 ID3v2 标签宿主（构造：msc.mp3 去掉 ID3 头）
    no_id3 = T.tmp("d_no_id3.mp3")
    with open(T.MP3, "rb") as f:
        data = f.read()
    rest = data[audio_start(data):]
    open(no_id3, "wb").write(rest)
    T.check("构造无标签 MP3 成功（非 ID3 开头）", rest[:3] != b"ID3" and len(rest) > 1024 * 1024,
            "大小=%d" % len(rest))
    payload2 = T.tmp("d_mp3_tagless.bin")
    T.make_random(payload2, 4 * 1024)
    steg2 = T.tmp("d_steg_tagless.mp3")
    outdir2 = T.tmp("d_out2")
    roundtrip(payload2, steg2, outdir2, host=no_id3)
    if os.path.exists(steg2):
        st2 = open(steg2, "rb").read()
        offs2 = scan_frames(st2)
        n_extra, masks = frame_data_diff(rest, st2, offs2)
        T.check("无标签宿主帧数据区零改动", n_extra == 0, "帧头外差异=%d 字节" % n_extra)

    # D4: 中文文件名载荷（MP3）
    cn = T.tmp("测试音频载荷.bin")
    T.make_random(cn, 2 * 1024)
    steg3 = T.tmp("d_steg_cn.mp3")
    outdir3 = T.tmp("d_out3")
    roundtrip(cn, steg3, outdir3)

    # D5: 容量超限 → 非 0 退出、无输出
    big = T.tmp("d_too_big.bin")
    T.make_random(big, 9 * 1024)
    rc, _, se = T.run(["embed", T.MP3, big, T.tmp("d_nope.mp3"), PW])
    T.check("容量超限 embed → 非0退出且无输出", rc != 0 and not os.path.exists(T.tmp("d_nope.mp3")),
            "rc=%d err=%s" % (rc, se.strip()))

    # E: has 检测
    rc, so, _ = T.run(["has", T.MP3, PW])
    T.check("has 干净 msc.mp3 → 0", rc == 0 and so.strip() == "0", "rc=%d out=%r" % (rc, so.strip()))
    if os.path.exists(steg):
        rc, so, _ = T.run(["has", steg, PW])
        T.check("has 嵌入后 steg.mp3 → 1", rc == 0 and so.strip() == "1", "rc=%d out=%r" % (rc, so.strip()))
        rc, so, _ = T.run(["has", steg, "wrong-password"])
        T.check("has 密码错误 → 0（无魔数，需 GCM 认证）", rc == 0 and so.strip() == "0",
                "rc=%d out=%r" % (rc, so.strip()))
    rnd = T.tmp("d_random.mp3")
    T.make_random(rnd, 200000)
    rc, so, _ = T.run(["has", rnd, PW])
    T.check("has 随机二进制改名 .mp3 → 0（不误报）", rc == 0 and so.strip() == "0",
            "rc=%d out=%r" % (rc, so.strip()))


if __name__ == "__main__":
    main()
    sys.exit(0 if T.summary("test_mp3") else 1)
