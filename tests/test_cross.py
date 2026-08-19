# -*- coding: utf-8 -*-
"""Section B: C++ 信封 ↔ Python envelope.py 双向交叉验证（字节级兼容）
可重跑：python tests/test_cross.py"""
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tester as T

# 使用本地 tests/envelope.py（参考实现副本）
_LOCAL_ENV = os.path.join(os.path.dirname(os.path.abspath(__file__)), "envelope.py")
if not os.path.exists(_LOCAL_ENV):
    raise SystemExit("missing tests/envelope.py (envelope reference implementation)")
ENV_PY_DIR = os.path.dirname(_LOCAL_ENV)
sys.path.insert(0, ENV_PY_DIR)
import envelope

MAGIC = b"CREEPER1"
HEADER_LEN = 41


def check_header(env_bytes, label):
    """校验 41 字节信封头布局"""
    ok = (len(env_bytes) >= HEADER_LEN
          and env_bytes[:8] == MAGIC
          and env_bytes[8] == 1
          and len(env_bytes[9:25]) == 16   # salt
          and len(env_bytes[25:37]) == 12  # nonce
          )
    ct_len = struct.unpack(">I", env_bytes[37:41])[0]
    ok = ok and len(env_bytes) == HEADER_LEN + ct_len
    T.check("信封头布局合法(%s)" % label, ok,
            "len=%d ct_len=%d magic=%s ver=%d" % (len(env_bytes), ct_len,
                                                  env_bytes[:8], env_bytes[8]))
    return ok


def main():
    T.reset()
    cases = [
        (64 * 1024, "cross-pass"),
        (1024 * 1024, "交叉密码-中文"),
    ]
    for sz, pw in cases:
        tag = "cn" if any(ord(c) > 127 for c in pw) else "ascii"
        payload = T.tmp("b_payload_%d_%s.bin" % (sz, tag))
        T.make_random(payload, sz)
        with open(payload, "rb") as f:
            payload_bytes = f.read()

        # B1: C++ seal → Python open_seal
        cpp_env = T.tmp("b_cpp_%d_%s.env" % (sz, tag))
        rc, _, se = T.run(["seal", payload, cpp_env, pw])
        cpp_env_b = open(cpp_env, "rb").read()
        if rc == 0 and check_header(cpp_env_b, "C++ seal"):
            try:
                got = envelope.open_seal(cpp_env_b, pw)
                T.check("C++ seal → Python open_seal 还原一致 (%dB %s)" % (sz, tag),
                        got == payload_bytes, "信封=%d字节" % len(cpp_env_b))
            except ValueError as e:
                T.check("C++ seal → Python open_seal 还原一致 (%dB %s)" % (sz, tag),
                        False, "open_seal 抛错: %s" % e)
        else:
            T.check("C++ seal → Python open_seal 还原一致 (%dB %s)" % (sz, tag),
                    False, "seal_rc=%d" % rc)

        # B2: Python seal → C++ open
        py_env_b = envelope.seal(payload_bytes, pw)
        py_env = T.tmp("b_py_%d_%s.env" % (sz, tag))
        open(py_env, "wb").write(py_env_b)
        out = T.tmp("b_py_%d_%s.out" % (sz, tag))
        rc2, _, se2 = T.run(["open", py_env, out, pw])
        ok = (rc2 == 0 and os.path.exists(out) and T.sha256(payload) == T.sha256(out))
        T.check("Python seal → C++ open 还原一致 (%dB %s)" % (sz, tag), ok,
                "rc=%d 信封=%d字节" % (rc2, len(py_env_b)))
        if ok:
            check_header(py_env_b, "Python seal")

    # B3: envelope.py 自检
    p = subprocess.run([sys.executable, os.path.join(ENV_PY_DIR, "envelope.py")],
                       capture_output=True, timeout=120)
    T.check("envelope.py 自检通过", p.returncode == 0,
            "rc=%d out=%r" % (p.returncode, p.stdout.decode("utf-8", "replace").strip()))


if __name__ == "__main__":
    main()
    sys.exit(0 if T.summary("test_cross") else 1)
