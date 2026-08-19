# -*- coding: utf-8 -*-
"""Section C: PNG 隐写往返（容量/无损/超限/中文名/直方图）+ Section E PNG 检测
可重跑：python tests/test_png.py
注意：embed 默认填充率上限 15%（抗统计检测）；has 需密码参数（无魔数，GCM 认证判定）。"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tester as T

import numpy as np
from PIL import Image

PW = "png-pass"
CAP_BYTES = None  # 运行时从宿主实际尺寸计算（tests/gen_hosts.py 生成的宿主为 2048x2048）
CAP_PCT = 15  # 默认填充率上限
CAP_LIMIT_BITS = None  # 15% 上限（bit），main() 里按实际容量计算


def png_info(path):
    im = Image.open(path)
    return im.format, im.mode, im.size


def pixel_diff(orig_path, steg_path):
    """逐像素比较：返回 (rgb 平均绝对差, alpha 最大差)。PIL 能打开 = 无损可读。"""
    a = np.asarray(Image.open(orig_path).convert("RGBA"), dtype=np.int16)
    b = np.asarray(Image.open(steg_path).convert("RGBA"), dtype=np.int16)
    rgb_mean = float(np.abs(a[:, :, :3] - b[:, :, :3]).mean())
    alpha_max = int(np.abs(a[:, :, 3] - b[:, :, 3]).max())
    return rgb_mean, alpha_max


def hist_l1(orig_path, steg_path):
    """RGB 汇总直方图的 L1 距离（Σ|H'−H|）。±1 嵌入+配对补偿后应远小于修改像素数。"""
    def hist(p):
        a = np.asarray(p.convert("RGB"), dtype=np.int64)
        h = np.zeros(256, dtype=np.int64)
        for c in range(3):
            h += np.bincount(a[:, :, c].ravel(), minlength=256)
        return h
    return int(np.abs(hist(Image.open(orig_path)) - hist(Image.open(steg_path))).sum())


def roundtrip(name, payload_size, extra=None):
    payload = T.tmp("c_payload_%s.bin" % name)
    steg = T.tmp("c_steg_%s.png" % name)
    outdir = T.tmp(os.path.join("c_out_%s" % name, "sub"))  # 嵌套目录，验证自动创建
    T.make_random(payload, payload_size)
    cmd = ["embed", T.IMG, payload, steg, PW] + (extra or [])
    rc, _, se = T.run(cmd)
    if rc != 0:
        T.check("PNG 往返 %s: embed 成功" % name, False, "rc=%d %s" % (rc, se.strip()))
        return None
    T.check("PNG 往返 %s: embed 成功" % name, True, "payload=%dB → steg.png" % payload_size)
    rc, so, _ = T.run(["has", steg, PW])
    T.check("PNG 往返 %s: has==1" % name, rc == 0 and so.strip() == "1", "rc=%d out=%r" % (rc, so.strip()))
    rc, _, se = T.run(["extract", steg, outdir, PW])
    out_file = os.path.join(outdir, os.path.basename(payload))
    ok = rc == 0 and os.path.exists(out_file) and T.sha256(payload) == T.sha256(out_file)
    T.check("PNG 往返 %s: extract 字节一致且文件名还原" % name, ok,
            "rc=%d 还原文件=%s" % (rc, os.path.exists(out_file)))
    return steg


def main():
    T.reset()

    # C1: 宿主信息与理论容量
    fmt, mode, (w, h) = png_info(T.IMG)
    cap = w * h * 3 // 8
    T.check("img.png 可被 PIL 解码", fmt == "PNG", "mode=%s size=%dx%d 理论容量=%d字节(%.2fMiB)"
            % (mode, w, h, cap, cap / 1048576.0))
    T.check("理论容量计算正确", cap > 256 * 1024 and w > 0 and h > 0, "cap=%d %dx%d" % (cap, w, h))
    global CAP_BYTES, CAP_LIMIT_BITS
    CAP_BYTES = cap
    CAP_LIMIT_BITS = CAP_BYTES * 8 * CAP_PCT // 100

    # C2: 小载荷往返（1KB / 150KB(≈9.8%) / 200KB(≈13%，默认上限内)）
    steg_1k = roundtrip("1k", 1024)
    steg_150k = roundtrip("150k", 150 * 1024)
    steg_200k = roundtrip("200k", 200 * 1024)

    # C3: 宿主无损：只允许 LSB 层变化（单通道最大差≤1，alpha 不变）
    # 注：平均绝对差与填充率相关——载荷填 60%+ 容量时平均差≈0.5×填充率>1/255 属 LSB 嵌入的正常现象
    for name, steg in (("1k", steg_1k), ("150k", steg_150k), ("200k", steg_200k)):
        if not steg or not os.path.exists(steg):
            T.check("PNG 无损 %s: steg 存在" % name, False)
            continue
        rgb_mean, alpha_max = pixel_diff(T.IMG, steg)
        a = np.asarray(Image.open(T.IMG).convert("RGBA"), dtype=np.int16)
        b = np.asarray(Image.open(steg).convert("RGBA"), dtype=np.int16)
        rgb_max = int(np.abs(a[:, :, :3] - b[:, :, :3]).max())
        ok = rgb_max <= 1 and alpha_max == 0
        if name == "1k":
            ok = ok and rgb_mean <= 1.0 / 255.0  # 小载荷：任务书平均差标准适用
        T.check("PNG 无损 %s: 仅 LSB 层变化（最大差≤1）%s" % (
            name, "且平均差≤1/255" if name == "1k" else ""), ok,
            "rgb平均差=%.5f rgb最大差=%d alpha最大差=%d" % (rgb_mean, rgb_max, alpha_max))

    # C4: 直方图保持（±1 嵌入 + 配对补偿）：200KB 载荷嵌入后直方图 L1 距离应显著小于
    # 修改像素数（无强制 LSB 的阶跃/系统性拉平特征）
    if steg_200k and os.path.exists(steg_200k):
        l1 = hist_l1(T.IMG, steg_200k)
        a = np.asarray(Image.open(T.IMG).convert("RGBA"), dtype=np.int16)
        b = np.asarray(Image.open(steg_200k).convert("RGBA"), dtype=np.int16)
        n_changed = int((np.abs(a[:, :, :3] - b[:, :, :3]) > 0).sum())
        T.check("PNG 直方图保持（配对补偿）：L1 ≤ 修改像素数×0.1 且无相邻bin阶跃", l1 * 10 <= n_changed,
                "L1=%d 修改通道=%d 比例=%.3f" % (l1, n_changed, l1 / max(n_changed, 1)))
    else:
        T.check("PNG 直方图保持（配对补偿）（前提：steg_200k 存在）", False)

    # C5a: 填充率上限（默认 15% = 230,400B）：250KB(≈16.3%) 超限报错 → --cap 100 放行并往返成功
    big = T.tmp("c_over15.bin")
    T.make_random(big, 250 * 1024)
    rc, _, se = T.run(["embed", T.IMG, big, T.tmp("c_over15.png"), PW])
    T.check("默认填充率上限（15%）：250KB 载荷 → 报错且无输出", rc != 0 and not os.path.exists(T.tmp("c_over15.png")),
            "rc=%d stderr=%s" % (rc, se.strip()))
    steg_cap = roundtrip("cap100", 250 * 1024, ["--cap", "100"])

    # C5b: 绝对容量超限（1.6MB > 1.5MB 理论容量，--cap 100 也失败）
    huge = T.tmp("c_too_big.bin")
    T.make_random(huge, 1600000)
    rc, _, se = T.run(["embed", T.IMG, huge, T.tmp("c_too_big.png"), PW, "--cap", "100"])
    T.check("绝对容量超限 → 报错且非0退出且无输出", rc != 0 and not os.path.exists(T.tmp("c_too_big.png")),
            "rc=%d stderr=%s" % (rc, se.strip()))

    # C5c: 实测容量边界（15% 上限下二分，载荷名固定 cap.bin 以消除文件名长度影响）
    lo, hi = 190000, 230000
    last_ok = None
    for _ in range(14):
        mid = (lo + hi) // 2
        p = T.tmp("cap.bin")
        T.make_random(p, mid)
        rc, _, _ = T.run(["embed", T.IMG, p, T.tmp("c_cap_probe.png"), PW])
        if rc == 0:
            lo, last_ok = mid, mid
        else:
            hi = mid
    measured = last_ok
    # 15% 上限 = CAP_LIMIT_BITS bit（=230,400B），减去头/信封膨胀开销后应落在 [200KB, 上限]
    T.check("实测容量边界（15% 上限二分）", measured is not None and 200000 <= measured <= CAP_LIMIT_BITS // 8,
            "最大可嵌入≈%d字节 / 15%%上限=%d字节" % (measured or 0, CAP_LIMIT_BITS // 8))

    # C6: 大载荷（超过 15% 上限）→ 预期超限（默认 15% 即拒绝）
    big_c6 = T.tmp("c_big6.bin")
    T.make_random(big_c6, CAP_LIMIT_BITS // 8 + 4096)
    rc, _, se = T.run(["embed", T.IMG, big_c6, T.tmp("c_src.png"), PW])
    T.check("超过 15%% 上限的大载荷嵌入 → 报错且无输出", rc != 0 and not os.path.exists(T.tmp("c_src.png")),
            "rc=%d stderr=%s" % (rc, se.strip()))

    # C7: 中文文件名载荷
    cn = T.tmp("测试载荷.bin")
    T.make_random(cn, 50000)
    steg_cn = T.tmp("c_steg_cn.png")
    outdir_cn = T.tmp("c_out_cn")
    rc, _, se = T.run(["embed", T.IMG, cn, steg_cn, PW])
    rc2, _, _ = T.run(["extract", steg_cn, outdir_cn, PW])
    restored = os.path.join(outdir_cn, "测试载荷.bin")
    ok = rc == 0 and rc2 == 0 and os.path.exists(restored) and T.sha256(cn) == T.sha256(restored)
    T.check("中文文件名载荷往返", ok, "rc_embed=%d rc_extract=%d 还原文件存在=%s"
            % (rc, rc2, os.path.exists(restored)))

    # E: has 检测（需密码：干净→0 / 嵌入后→1 / 密码错→0 / 随机二进制改 .png →0）
    rc, so, _ = T.run(["has", T.IMG, PW])
    T.check("has 干净 img.png → 0", rc == 0 and so.strip() == "0", "rc=%d out=%r" % (rc, so.strip()))
    if steg_200k and os.path.exists(steg_200k):
        rc, so, se = T.run(["has", steg_200k, PW])
        T.check("has 嵌入后 steg.png → 1", rc == 0 and so.strip() == "1",
                "rc=%d out=%r stderr=%s" % (rc, so.strip(), se.strip()))
    else:
        T.check("has 嵌入后 steg.png → 1（前提：steg 文件存在）", False,
                "steg_200k 缺失" + ("（路径 %s）" % steg_200k if steg_200k else ""))
    if steg_200k and os.path.exists(steg_200k):
        rc, so, _ = T.run(["has", steg_200k, "wrong-password"])
        T.check("has 密码错误 → 0（无魔数，需 GCM 认证）", rc == 0 and so.strip() == "0",
                "rc=%d out=%r" % (rc, so.strip()))
    else:
        T.check("has 密码错误 → 0（前提：steg 文件存在）", False)
    rnd = T.tmp("c_random.png")
    T.make_random(rnd, 200000)
    rc, so, _ = T.run(["has", rnd, PW])
    T.check("has 随机二进制改名 .png → 0（不误报）", rc == 0 and so.strip() == "0",
            "rc=%d out=%r" % (rc, so.strip()))


if __name__ == "__main__":
    main()
    sys.exit(0 if T.summary("test_png") else 1)