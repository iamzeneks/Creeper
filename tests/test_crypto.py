# -*- coding: utf-8 -*-
"""Section A: CLI seal/open 加密往返（含负例）
   Section G: CLI 健壮性（缺参/文件不存在/空宿主/空载荷）
可重跑：python tests/test_crypto.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tester as T


def section_a():
    print("--- A. 加密往返 seal/open ---")
    sizes = [1, 1024, 1024 * 1024, 10 * 1024 * 1024]
    passwords = ["pass123", "测试密码-中文", ""]
    for sz in sizes:
        for pw in passwords:
            tag = "empty" if pw == "" else "cn" if any(ord(c) > 127 for c in pw) else "ascii"
            src = T.tmp("a_%d_%s.bin" % (sz, tag))
            env = T.tmp("a_%d_%s.env" % (sz, tag))
            out = T.tmp("a_%d_%s.out" % (sz, tag))
            T.make_random(src, sz)
            rc1, _, se1 = T.run(["seal", src, env, pw])
            rc2, _, se2 = T.run(["open", env, out, pw])
            ok = (rc1 == 0 and rc2 == 0 and os.path.exists(env)
                  and os.path.exists(out) and T.sha256(src) == T.sha256(out))
            T.check("往返 %dB 密码[%s]" % (sz, tag), ok,
                    "seal_rc=%d open_rc=%d" % (rc1, rc2))

    # A2 负例：错误密码
    src = T.tmp("a_wrong.bin")
    env = T.tmp("a_wrong.env")
    out = T.tmp("a_wrong.out")
    T.make_random(src, 65536)
    T.run(["seal", src, env, "right-pass"])
    rc, _, se = T.run(["open", env, out, "wrong-pass"])
    T.check("错误密码 → 非0退出且不产生输出文件", rc != 0 and not os.path.exists(out),
            "rc=%d stderr=%s" % (rc, se.strip()))

    # A3 负例：篡改信封 1 bit（密文中部 / 头部 magic）
    env_b = bytearray(open(env, "rb").read())
    env_t = T.tmp("a_tamper_ct.env")
    open(env_t, "wb").write(env_b[: len(env_b) // 2] + bytes([env_b[len(env_b) // 2] ^ 0x01]) + env_b[len(env_b) // 2 + 1:])
    rc, _, se = T.run(["open", env_t, T.tmp("a_tamper_ct.out"), "right-pass"])
    T.check("篡改密文中部 1bit → 非0退出且无输出", rc != 0 and not os.path.exists(T.tmp("a_tamper_ct.out")),
            "rc=%d stderr=%s" % (rc, se.strip()))

    env_h = T.tmp("a_tamper_hdr.env")
    open(env_h, "wb").write(bytes([env_b[0] ^ 0x01]) + bytes(env_b[1:]))
    rc, _, se = T.run(["open", env_h, T.tmp("a_tamper_hdr.out"), "right-pass"])
    T.check("篡改头部 magic 1bit → 非0退出且无输出", rc != 0 and not os.path.exists(T.tmp("a_tamper_hdr.out")),
            "rc=%d stderr=%s" % (rc, se.strip()))


def section_g():
    print("--- G. 健壮性 ---")
    # 缺参数
    rc, _, _ = T.run([])
    T.check("无参数 → 非0退出", rc != 0, "rc=%d" % rc)
    rc, _, _ = T.run(["seal"])
    T.check("seal 缺参数 → 非0退出", rc != 0, "rc=%d" % rc)
    rc, _, _ = T.run(["embed", "a", "b", "c"])
    T.check("embed 缺参数 → 非0退出", rc != 0, "rc=%d" % rc)

    # 文件不存在
    rc, _, se = T.run(["seal", T.tmp("no_such.bin"), T.tmp("x.env"), "pw"])
    T.check("seal 输入不存在 → 非0退出", rc != 0, "rc=%d" % rc)
    rc, _, se = T.run(["open", T.tmp("no_such.env"), T.tmp("x.out"), "pw"])
    T.check("open 信封不存在 → 非0退出", rc != 0, "rc=%d" % rc)
    rc, _, se = T.run(["has", T.tmp("no_such.png"), "pw"])
    T.check("has 文件不存在 → 非0退出", rc != 0, "rc=%d" % rc)
    rc, _, se = T.run(["extract", T.tmp("no_such.mp3"), T.tmp("od"), "pw"])
    T.check("extract 文件不存在 → 非0退出", rc != 0, "rc=%d" % rc)

    # 空宿主
    empty_png = T.tmp("empty.png")
    empty_mp3 = T.tmp("empty.mp3")
    empty_payload = T.tmp("empty.bin")
    payload = T.tmp("g_payload.bin")
    T.make_random(payload, 4096)
    for f in (empty_png, empty_mp3, empty_payload):
        open(f, "wb").close()
    rc, _, se = T.run(["embed", empty_png, payload, T.tmp("g_out.png"), "pw"])
    T.check("embed 空 PNG 宿主 → 非0退出且无输出", rc != 0 and not os.path.exists(T.tmp("g_out.png")),
            "rc=%d stderr=%s" % (rc, se.strip()))
    rc, _, se = T.run(["embed", empty_mp3, payload, T.tmp("g_out.mp3"), "pw"])
    T.check("embed 空 MP3 宿主 → 非0退出且无输出", rc != 0 and not os.path.exists(T.tmp("g_out.mp3")),
            "rc=%d stderr=%s" % (rc, se.strip()))
    rc, so, se = T.run(["has", empty_png, "pw"])
    T.check("has 空 PNG → 输出 0（空文件无载荷，rc=0；OBS-1 语义正式化）",
            rc == 0 and so.strip() == "0", "rc=%d out=%r stderr=%s" % (rc, so.strip(), se.strip()))

    # 空载荷
    rc, _, se = T.run(["embed", T.IMG, empty_payload, T.tmp("g_out2.png"), "pw"])
    T.check("embed 空载荷 → 非0退出且无输出", rc != 0 and not os.path.exists(T.tmp("g_out2.png")),
            "rc=%d stderr=%s" % (rc, se.strip()))
    rc, _, se = T.run(["embed", T.MP3, empty_payload, T.tmp("g_out2.mp3"), "pw"])
    T.check("embed 空载荷(MP3) → 非0退出且无输出", rc != 0 and not os.path.exists(T.tmp("g_out2.mp3")),
            "rc=%d stderr=%s" % (rc, se.strip()))

    # 未知扩展名宿主 / open 垃圾文件
    bad = T.tmp("g_host.txt")
    open(bad, "w").write("hello")
    rc, _, se = T.run(["embed", bad, payload, T.tmp("g_out3"), "pw"])
    T.check("embed 非 png/mp3 宿主 → 非0退出", rc != 0, "rc=%d" % rc)
    rc, _, se = T.run(["has", bad, "pw"])
    T.check("has 非 png/mp3 宿主 → 非0退出", rc != 0, "rc=%d" % rc)
    junk = T.tmp("g_junk.env")
    T.make_random(junk, 1024)
    rc, _, se = T.run(["open", junk, T.tmp("g_junk.out"), "pw"])
    T.check("open 随机垃圾文件 → 非0退出且无输出", rc != 0 and not os.path.exists(T.tmp("g_junk.out")),
            "rc=%d stderr=%s" % (rc, se.strip()))

    # 空宿主 extract
    rc, _, se = T.run(["extract", empty_png, T.tmp("g_od"), "pw"])
    T.check("extract 空 PNG → 非0退出", rc != 0, "rc=%d" % rc)


if __name__ == "__main__":
    T.reset()
    section_a()
    section_g()
    sys.exit(0 if T.summary("test_crypto") else 1)
