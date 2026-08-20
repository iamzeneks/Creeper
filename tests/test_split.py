# -*- coding: utf-8 -*-
"""test_split.py — 大文件拆分多宿主（split/unsplit）往返 + 健壮性

依赖：src/build.bat 已构建（res/creeper_cli.exe）、assets/ 宿主已生成。
覆盖：混合宿主往返、乱序提取、缺块/密码错/单块不可解、容量不足、WAV depth、命名。
每个用例使用独立宿主副本 + 独立输出目录，避免输出文件互相覆盖。
"""
import os
import random
import shutil
import sys

sys.path.insert(0, os.path.dirname(__file__))
import tester

PW = "split-secret-2026"

random.seed(20260819)


def payload_bytes(n):
    return bytes(random.randrange(256) for _ in range(n))


def make_hosts(prefix):
    """独立宿主副本：[png, png, wav, mp3]"""
    out = []
    for src, ext in ((tester.IMG, "png"), (tester.IMG, "png"), (tester.WAV, "wav"), (tester.MP3, "mp3")):
        p = tester.tmp("%s_h%d.%s" % (prefix, len(out), ext))
        shutil.copy(src, p)
        out.append(p)
    return out


def run_split(payload, hosts, outdir, cap=None, depth=None):
    args = ["split", payload, PW, outdir] + hosts
    if cap is not None:
        args += ["--cap", str(cap)]
    if depth is not None:
        args += ["--depth", str(depth)]
    return tester.run(args)


def run_unsplit(hosts, outdir, pw=PW):
    return tester.run(["unsplit", outdir, pw] + hosts)


def main():
    tester.reset()
    d = tester.TMP

    # ---- S1：2 PNG + 1 WAV，15%，500KB 随机载荷 → 3 块 ----
    h = make_hosts("s1")
    p1 = tester.tmp("s1_payload.bin")
    with open(p1, "wb") as f:
        f.write(payload_bytes(500 * 1024))
    out1 = os.path.join(d, "s1")
    rc, so, se = run_split(p1, h[:3], out1)
    tester.check("S1 split 混合宿主(2png+wav) 成功", rc == 0, se.strip())
    o1, o2, ow = (os.path.join(out1, n) for n in ("s1_h0_已转换.png", "s1_h1_已转换.png", "s1_h2_已转换.wav"))
    tester.check("S1 输出文件齐备（宿主_已转换.ext）",
                 os.path.exists(o1) and os.path.exists(o2) and os.path.exists(ow))
    rc, so, se = run_unsplit([o1, o2, ow], out1)
    out = os.path.join(out1, "s1_payload.bin")
    tester.check("S1 顺序还原字节一致", rc == 0 and tester.sha256(out) == tester.sha256(p1), se.strip())

    # ---- S2：乱序提取（块内编号拼接，与宿主顺序无关） ----
    rc, so, se = run_unsplit([ow, o1, o2], out1)
    out = os.path.join(out1, "s1_payload.bin")
    tester.check("S2 乱序 unsplit 还原一致", rc == 0 and tester.sha256(out) == tester.sha256(p1), se.strip())

    # ---- S3：PNG+WAV+MP3 全混合，250KB 载荷（PNG 块 + WAV 块，MP3 仅当总量不足时参与） ----
    h = make_hosts("s3")
    p3 = tester.tmp("s3_payload.bin")
    with open(p3, "wb") as f:
        f.write(payload_bytes(250 * 1024))
    out3 = os.path.join(d, "s3")
    rc, so, se = run_split(p3, [h[0], h[2], h[3]], out3)
    tester.check("S3 split PNG+WAV+MP3 成功", rc == 0, se.strip())
    rc, so, se = run_unsplit(
        [os.path.join(out3, "s3_h3_已转换.mp3"), os.path.join(out3, "s3_h2_已转换.wav"),
         os.path.join(out3, "s3_h0_已转换.png")], out3)
    out = os.path.join(out3, "s3_payload.bin")
    tester.check("S3 全混合乱序还原一致",
                 rc == 0 and tester.sha256(out) == tester.sha256(p3), se.strip())

    # ---- S4：缺块 → 失败 ----
    rc, so, se = run_unsplit([o1, o2], out1)
    tester.check("S4 缺块 unsplit 失败", rc != 0 and "missing" in se, se.strip())

    # ---- S5：密码错 → 失败 ----
    rc, so, se = run_unsplit([o1, o2, ow], out1, pw="wrong-password")
    tester.check("S5 密码错 unsplit 失败", rc != 0, se.strip())

    # ---- S6：单块不可解——普通 extract/has 对分片块必须视为无载荷 ----
    rc, so, se = tester.run(["extract", o1, out1, PW])
    tester.check("S6 单块 extract 失败（信息不完整=无载荷）", rc != 0, se.strip())
    rc, so, se = tester.run(["has", o1, PW])
    tester.check("S6 单块 has=0", rc == 0 and so.strip() == "0", se.strip())

    # ---- S7：载荷超过宿主总和 → split 报错 ----
    h = make_hosts("s7")
    big = tester.tmp("s7_big.bin")
    tester.make_random(big, 600 * 1024)  # 2 PNG 15% ≈ 471KB < 600KB
    rc, so, se = run_split(big, h[:2], os.path.join(d, "s7"))
    tester.check("S7 超总和容量 split 失败", rc != 0 and "combined host capacity" in se, se.strip())

    # ---- S8：WAV depth=2 参与分片（容量 ×2），500KB 载荷 → PNG(30%) + WAV(30%) 两块 ----
    h = make_hosts("s8")
    p8 = tester.tmp("s8_payload.bin")
    with open(p8, "wb") as f:
        f.write(payload_bytes(500 * 1024))
    out8 = os.path.join(d, "s8")
    rc, so, se = run_split(p8, [h[0], h[2]], out8, cap=30, depth=2)
    tester.check("S8 split --depth 2 --cap 30 成功", rc == 0, se.strip())
    tester.check("S8 两块输出（PNG+WAV）", os.path.exists(os.path.join(out8, "s8_h0_已转换.png"))
                 and os.path.exists(os.path.join(out8, "s8_h2_已转换.wav")))
    rc, so, se = run_unsplit(
        [os.path.join(out8, "s8_h2_已转换.wav"), os.path.join(out8, "s8_h0_已转换.png")], out8)
    out = os.path.join(out8, "s8_payload.bin")
    tester.check("S8 depth2 乱序还原一致",
                 rc == 0 and tester.sha256(out) == tester.sha256(p8), se.strip())

    # ---- S9：空密码往返（CLI 允许；GUI 无密码不触发隐写路径） ----
    h = make_hosts("s9")
    p9 = tester.tmp("s9_payload.bin")
    with open(p9, "wb") as f:
        f.write(payload_bytes(200 * 1024))
    out9 = os.path.join(d, "s9")
    rc, so, se = tester.run(["split", p9, "", out9, h[0]])
    tester.check("S9 split 空密码成功", rc == 0, se.strip())
    rc, so, se = tester.run(["unsplit", out9, "", os.path.join(out9, "s9_h0_已转换.png")])
    out = os.path.join(out9, "s9_payload.bin")
    tester.check("S9 空密码还原一致",
                 rc == 0 and tester.sha256(out) == tester.sha256(p9), se.strip())

    # ---- S10：无分片宿主（普通单宿主载荷）→ unsplit 无载荷失败 ----
    p10 = tester.tmp("s10_payload.bin")
    with open(p10, "wb") as f:
        f.write(payload_bytes(8 * 1024))
    rc, so, se = tester.run(["embed", tester.IMG, p10, tester.tmp("s10_embed.png"), PW])
    tester.check("S10 预置普通单宿主载荷", rc == 0, se.strip())
    rc, so, se = run_unsplit([tester.tmp("s10_embed.png")], os.path.join(d, "s10"))
    tester.check("S10 普通载荷文件 unsplit 失败（非分片）", rc != 0, se.strip())

    # ---- S11：单宿主 split（count=1，GUI 2 文件嵌入产物）——
    # 必须能由 unsplit 单宿主还原；普通 extract 对其必须失败（GUI-6 修复回归）。
    p11 = tester.tmp("s11_payload.bin")
    with open(p11, "wb") as f:
        f.write(payload_bytes(8 * 1024))
    h11 = tester.tmp("s11_host.wav")
    shutil.copy(tester.WAV, h11)
    out11 = os.path.join(d, "s11")
    rc, so, se = run_split(p11, [h11], out11)
    tester.check("S11 单宿主 split 成功", rc == 0, se.strip())
    o11 = os.path.join(out11, "s11_host_已转换.wav")
    tester.check("S11 输出文件齐备（宿主_已转换.wav）", os.path.exists(o11))
    out11b = os.path.join(out11, "s11_payload.bin")
    rc, so, se = run_unsplit([o11], out11)
    tester.check("S11 单宿主 unsplit 还原一致（GUI 单文件提取路径）",
                 rc == 0 and tester.sha256(out11b) == tester.sha256(p11), se.strip())
    rc, so, se = tester.run(["extract", o11, os.path.join(out11, "plain"), PW])
    tester.check("S11 单宿主分片文件普通 extract 失败（格式分离）", rc != 0, se.strip())

    ok = tester.summary("split")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()