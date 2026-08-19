# -*- coding: utf-8 -*-
"""creeper 测试公共工具：路径、CLI 封装、断言、tmp 管理"""
import datetime
import hashlib
import io
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TMP = os.path.join(ROOT, "tests", "tmp")
CLI = os.path.join(ROOT, "creeper_cli.exe")
IMG = os.path.join(ROOT, "assets", "img.png")
MP3 = os.path.join(ROOT, "assets", "msc.mp3")
WAV = os.path.join(ROOT, "assets", "test.wav")

_passed = 0
_failed = 0
_failures = []
_buf = io.StringIO()


def _emit(s):
    _buf.write(s + "\n")
    print(s)


def reset():
    global _passed, _failed, _failures
    _passed = 0
    _failed = 0
    _failures = []
    if os.path.isdir(TMP):
        shutil.rmtree(TMP)
    os.makedirs(TMP, exist_ok=True)


def tmp(name):
    return os.path.join(TMP, name)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def make_random(path, n):
    with open(path, "wb") as f:
        f.write(os.urandom(n))


def run(args, timeout=900):
    """运行 CLI，返回 (exit_code, stdout, stderr)"""
    p = subprocess.run([CLI] + args, capture_output=True, timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def check(name, ok, detail=""):
    global _passed, _failed
    if ok:
        _passed += 1
        _emit("  [PASS] " + name + (("  -- " + detail) if detail else ""))
    else:
        _failed += 1
        _failures.append((name, detail))
        _emit("  [FAIL] " + name + (("  -- " + detail) if detail else ""))


def summary(section):
    line = "== %s: %d passed, %d failed ==" % (section, _passed, _failed)
    _emit(line)
    if _failures:
        for name, detail in _failures:
            _emit("  failure: %s -- %s" % (name, detail))
    res_dir = os.path.join(ROOT, "tests", "results")
    os.makedirs(res_dir, exist_ok=True)
    path = os.path.join(res_dir, "res_%s.txt" % section)
    with open(path, "w", encoding="utf-8") as f:
        f.write(_buf.getvalue())
        f.write("\n[archived %s]\n" % datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
    return _failed == 0
